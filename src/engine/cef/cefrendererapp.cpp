#include "engine/cef/cefrendererapp.h"
#include "engine/mediaactivityscript.h"

#include <cstdlib>
#include <ctime>

#include "include/cef_command_line.h"
#include "include/cef_v8.h"

namespace eden::engine::cef {

    class DisplayCaptureRequestHandler final : public CefV8Handler {
      public:
        explicit DisplayCaptureRequestHandler(CefRendererApp *app)
            : m_app(app) {}

        bool Execute(
            const CefString &name,
            CefRefPtr<CefV8Value>,
            const CefV8ValueList &arguments,
            CefRefPtr<CefV8Value> &returnValue,
            CefString &exception
        ) override {
            if (name == "finishDisplayCapture" && arguments.size() == 1 && arguments[0]->IsInt()) {
                const CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
                const CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
                if (frame) {
                    const CefRefPtr<CefProcessMessage> message =
                        CefProcessMessage::Create("eden_display_capture_finished");
                    message->GetArgumentList()->SetInt(0, arguments[0]->GetIntValue());
                    frame->SendProcessMessage(PID_BROWSER, message);
                }
                return true;
            }
            if (name != "requestDisplayCapture" || arguments.size() != 1 || !arguments[0]->IsBool()) {
                exception = "Invalid display capture request";
                return true;
            }
            return m_app->requestDisplayCapture(
                CefV8Context::GetCurrentContext(),
                arguments[0]->GetBoolValue(),
                returnValue,
                exception
            );
        }

      private:
        CefRendererApp *m_app;

        IMPLEMENT_REFCOUNTING(DisplayCaptureRequestHandler);
    };

    namespace {

        class MediaActivityHandler final : public CefV8Handler {
          public:
            bool Execute(
                const CefString &,
                CefRefPtr<CefV8Value>,
                const CefV8ValueList &arguments,
                CefRefPtr<CefV8Value> &,
                CefString &
            ) override {
                const auto context = CefV8Context::GetCurrentContext();
                const auto frame = context ? context->GetFrame() : nullptr;
                if (!frame || !context->IsSame(frame->GetV8Context()) || arguments.size() != 1 ||
                    !arguments[0]->IsInt()) {
                    return false;
                }
                const auto message = CefProcessMessage::Create("eden_media_activity");
                message->GetArgumentList()->SetInt(0, arguments[0]->GetIntValue() & 15);
                frame->SendProcessMessage(PID_BROWSER, message);
                return true;
            }
            IMPLEMENT_REFCOUNTING(MediaActivityHandler);
        };

        class ClipboardReportHandler final : public CefV8Handler {
          public:
            explicit ClipboardReportHandler(std::string documentId)
                : m_documentId(std::move(documentId)) {}

            bool Execute(
                const CefString &name,
                CefRefPtr<CefV8Value>,
                const CefV8ValueList &arguments,
                CefRefPtr<CefV8Value> &returnValue,
                CefString &
            ) override {
                if (name != "reportCopy" || arguments.size() > 2 || (!arguments.empty() && !arguments[0]->IsString()) ||
                    (arguments.size() == 2 && !arguments[1]->IsString())) {
                    return false;
                }
                CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
                CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
                if (!frame) {
                    return true;
                }
                CefRefPtr<CefProcessMessage> message;
                if (arguments.empty()) {
                    const std::string token = m_documentId + ":" + std::to_string(++m_nextOperation);
                    message = CefProcessMessage::Create("eden_clipboard_begin");
                    message->GetArgumentList()->SetString(0, token);
                    returnValue = CefV8Value::CreateString(token);
                } else {
                    message = CefProcessMessage::Create("eden_clipboard_copy");
                    message->GetArgumentList()->SetString(
                        0,
                        arguments.size() == 2 ? arguments[0]->GetStringValue() : CefString()
                    );
                    message->GetArgumentList()->SetString(1, arguments.back()->GetStringValue());
                }
                frame->SendProcessMessage(PID_BROWSER, message);
                return true;
            }

          private:
            std::string m_documentId;
            uint64_t m_nextOperation = 0;

            IMPLEMENT_REFCOUNTING(ClipboardReportHandler);
        };

        class FormReportHandler final : public CefV8Handler {
          public:
            bool Execute(
                const CefString &name,
                CefRefPtr<CefV8Value>,
                const CefV8ValueList &arguments,
                CefRefPtr<CefV8Value> &,
                CefString &
            ) override {
                CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
                CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
                if (!frame) {
                    return true;
                }
                if (name == "reportCredential" && arguments.size() == 2 && arguments[0]->IsString() &&
                    arguments[1]->IsString()) {
                    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_credential_submit");
                    message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
                    message->GetArgumentList()->SetString(1, arguments[1]->GetStringValue());
                    frame->SendProcessMessage(PID_BROWSER, message);
                    return true;
                }
                if (name == "reportFormField" && arguments.size() == 8 && arguments[0]->IsString() &&
                    arguments[1]->IsString() && arguments[2]->IsString() && arguments[3]->IsString()) {
                    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_form_field_focus");
                    for (int index = 0; index < 4; ++index) {
                        message->GetArgumentList()->SetString(index, arguments[index]->GetStringValue());
                    }
                    for (int index = 4; index < 8; ++index) {
                        message->GetArgumentList()->SetDouble(index, arguments[index]->GetDoubleValue());
                    }
                    frame->SendProcessMessage(PID_BROWSER, message);
                    return true;
                }
                return false;
            }

          private:
            IMPLEMENT_REFCOUNTING(FormReportHandler);
        };

        constexpr const char *kClipboardHookScript = R"JS((function(){
'use strict';
var report = window.__edenReportCopy;
delete window.__edenReportCopy;
if (!report) { return; }
var textValue = String;
var then = Function.prototype.call.bind(Promise.prototype.then);
var reject = Promise.reject.bind(Promise);
var arrayFrom = Array.from.bind(Array);
var arrayValues = Function.prototype.call.bind(Array.prototype.values);
var defineProperty = Object.defineProperty;
var iteratorKey = Symbol.iterator;
var getItemType = typeof ClipboardItem === 'function' ?
    Function.prototype.call.bind(ClipboardItem.prototype.getType) : null;
var blobText = typeof Blob === 'function' ? Function.prototype.call.bind(Blob.prototype.text) : null;
var forward = function(text){ try { report(textValue(text)); } catch (error) {} };
var grab = function(event){
    if (!event || !event.isTrusted) { return; }
    var text = '';
    try { if (event && event.clipboardData) { text = event.clipboardData.getData('text/plain') || ''; } } catch (error) {}
    if (!text) { try { var selection = window.getSelection(); text = selection ? selection.toString() : ''; } catch (error) {} }
    if (!text) {
        try {
            var active = document.activeElement;
            if (active && (active.tagName === 'INPUT' || active.tagName === 'TEXTAREA') && active.selectionStart !== active.selectionEnd) {
                text = active.value.substring(active.selectionStart, active.selectionEnd);
            }
        } catch (error) {}
    }
    forward(text);
};
window.addEventListener('copy', grab);
window.addEventListener('cut', grab);
if (navigator.clipboard) {
    if (navigator.clipboard.writeText) {
        var writeText = navigator.clipboard.writeText.bind(navigator.clipboard);
        navigator.clipboard.writeText = function(value){
            if (typeof value === 'symbol') { return writeText(value); }
            var text;
            try { text = textValue(value); } catch (error) { return reject(error); }
            var pending = writeText(text);
            then(pending, function(){ forward(text); }, function(){});
            return pending;
        };
    }
    if (navigator.clipboard.write) {
        var write = navigator.clipboard.write.bind(navigator.clipboard);
        navigator.clipboard.write = function(items){
            var snapshot;
            try {
                snapshot = arrayFrom(items);
                defineProperty(snapshot, iteratorKey, {value:function(){ return arrayValues(snapshot); }});
            } catch (error) { return reject(error); }
            var pending = write(snapshot);
            then(pending, function(){
                if (!snapshot.length) { return; }
                var token;
                try { token = report(); } catch (error) { return; }
                if (!getItemType || !blobText) { return; }
                try {
                    then(getItemType(snapshot[0], 'text/plain'), function(blob){
                        try {
                            then(blobText(blob), function(text){
                                try { report(token, textValue(text)); } catch (error) {}
                            }, function(){});
                        } catch (error) {}
                    }, function(){});
                } catch (error) {}
            }, function(){});
            return pending;
        };
    }
}
try {
    var frontendHost = window.InspectorFrontendHost;
    if (frontendHost && typeof frontendHost.copyText === 'function') {
        var copyText = frontendHost.copyText.bind(frontendHost);
        frontendHost.copyText = function(value){ var result = copyText(value); forward(value); return result; };
    }
} catch (error) {}
})();)JS";

        constexpr const char *kFormHookScript = R"JS((function(){
'use strict';
var reportCredential = window.__edenReportCredential;
var reportFormField = window.__edenReportFormField;
delete window.__edenReportCredential;
delete window.__edenReportFormField;
if (!reportCredential || !reportFormField) { return; }
var visible = function(field){ return field && !field.disabled && !field.readOnly && field.getClientRects().length > 0; };
var fieldsFor = function(root){ return Array.prototype.slice.call((root || document).querySelectorAll('input,textarea')); };
var autocompleteHas = function(field, token){ return String(field.autocomplete || '').toLowerCase().split(/\s+/).indexOf(token) >= 0; };
var fieldIdentity = function(field){
    return [field.name, field.id, field.placeholder, field.getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();
};
var usernameField = function(field){
    return field && field.value &&
        (autocompleteHas(field, 'username') || autocompleteHas(field, 'email') || field.type === 'email' ||
         /user|email|login|identifier/.test(fieldIdentity(field))) && visible(field);
};
var secretType = function(field){
    if (!field || autocompleteHas(field, 'one-time-code')) { return false; }
    if (field.type === 'password' || autocompleteHas(field, 'current-password') || autocompleteHas(field, 'new-password')) {
        return true;
    }
    return /password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-]*key/.test(fieldIdentity(field));
};
var secretField = function(field){ return field && field.value && secretType(field) && visible(field); };
var usernameCandidate = function(root){
    var field = fieldsFor(root).find(usernameField);
    return field ? field.value || '' : '';
};
var usernameFor = function(secret){
    var form = secret.form || document;
    var explicit = usernameCandidate(form);
    if (explicit) { return explicit; }
    var fields = fieldsFor(form);
    var index = fields.indexOf(secret);
    for (var position = index - 1; position >= 0; --position) {
        var field = fields[position];
        if (visible(field) && /^(text|email|tel|$)/.test(field.type || '')) { return field.value || ''; }
    }
    return '';
};
var capture = function(root){
    var secret = fieldsFor(root).find(secretField);
    var username = secret ? usernameFor(secret) : usernameCandidate(root);
    if (username || secret) { try { reportCredential(username, secret ? secret.value : ''); } catch (error) {} }
};
document.addEventListener('submit', function(event){ if (event.isTrusted) { capture(event.target); } }, true);
document.addEventListener('click', function(event){
    if (!event.isTrusted) { return; }
    var target = event.target && event.target.closest ? event.target.closest('button,input,[role="button"],a') : null;
    if (!target) { return; }
    var label = [target.textContent, target.value, target.getAttribute('aria-label')].filter(Boolean).join(' ');
    var root = target.form || (target.closest ? target.closest('form') : null) || document;
    var buttonLike = /^(BUTTON|INPUT)$/.test(target.tagName) || target.getAttribute('role') === 'button';
    var activatesLogin = /sign.?in|log.?in|continue|next|submit|authenticate|unlock/i.test(label);
    var secondaryControl = /show|hide|reveal|visibility|cancel|back/i.test(label);
    if (target.type === 'submit' || activatesLogin || (buttonLike && !secondaryControl && fieldsFor(root).some(secretField))) {
        capture(root);
    }
}, true);
document.addEventListener('change', function(event){
    if (!event.isTrusted) { return; }
    var field = event.target;
    if (field && /^(INPUT|TEXTAREA)$/.test(field.tagName) && usernameField(field)) {
        try { reportCredential(field.value || '', ''); } catch (error) {}
    }
}, true);
document.addEventListener('keydown', function(event){
    if (event.isTrusted && event.key === 'Enter') { capture(event.target.form || document); }
}, true);
var relevantField = function(field){
    if (!field || !/^(INPUT|TEXTAREA)$/.test(field.tagName) || field.disabled || field.readOnly) { return false; }
    if (/^(password|email|tel)$/.test(field.type)) { return true; }
    var identity = fieldIdentity(field) + ' ' + String(field.autocomplete || '').toLowerCase();
    return /name|email|user|login|identifier|pass|token|secret|access|phone|address|city|state|province|postal|zip|country/
        .test(identity);
};
var requestFrame = window.requestAnimationFrame.bind(window);
var cancelFrame = window.cancelAnimationFrame.bind(window);
var pendingFrame = null;
var pendingField = null;
var lastReport = null;
var clearField = function(){
    if (pendingFrame !== null) { cancelFrame(pendingFrame); }
    pendingFrame = null;
    pendingField = null;
    if (lastReport) {
        lastReport = null;
        try { reportFormField('', '', '', '', 0, 0, 0, 0); } catch (error) {}
    }
};
var flushField = function(){
    pendingFrame = null;
    var field = pendingField;
    pendingField = null;
    if (!relevantField(field) || document.activeElement !== field) { clearField(); return; }
    var rect = field.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) { clearField(); return; }
    var value = secretType(field) ? '' : field.value || '';
    var type = field.type || 'text';
    var name = field.name || field.id || '';
    var autocomplete = field.autocomplete || '';
    var values = [type, name, autocomplete, value, rect.x, rect.y, rect.width, rect.height];
    if (lastReport) {
        var changed = false;
        for (var index = 0; index < values.length; ++index) {
            if (values[index] !== lastReport[index]) { changed = true; break; }
        }
        if (!changed) { return; }
    }
    lastReport = values;
    try {
        reportFormField(type, name, autocomplete, value, rect.x, rect.y, rect.width, rect.height);
    } catch (error) {}
};
var queueField = function(event){
    if (!event.isTrusted) { return; }
    if (!relevantField(event.target)) { clearField(); return; }
    pendingField = event.target;
    if (pendingFrame === null) { pendingFrame = requestFrame(flushField); }
};
document.addEventListener('focusin', function(event){
    queueField(event);
}, true);
document.addEventListener('focusout', function(event){ if (event.isTrusted) { clearField(); } }, true);
document.addEventListener('input', function(event){
    queueField(event);
}, true);
window.addEventListener('blur', clearField);
})();)JS";

        constexpr const char *kDisplayCaptureHookScript = R"JS((function(){
var requestDisplayCapture = window.__edenRequestDisplayCapture;
var finishDisplayCapture = window.__edenFinishDisplayCapture;
var portalCapture = window.__edenPortalCapture === true;
delete window.__edenRequestDisplayCapture;
delete window.__edenFinishDisplayCapture;
delete window.__edenPortalCapture;
var mediaDevices = navigator.mediaDevices;
if (!requestDisplayCapture || !mediaDevices ||
    typeof mediaDevices.getDisplayMedia !== 'function') {
    return;
}

var nativeGetDisplayMedia = mediaDevices.getDisplayMedia.bind(mediaDevices);
var delegatedSourceId = function(){
    var values = new Uint32Array(1);
    if (window.crypto && typeof window.crypto.getRandomValues === 'function') {
        window.crypto.getRandomValues(values);
    }
    return String(values[0] || Math.floor(Date.now() % 2147483646) + 1);
};
var snapshotConstraints = function(requested, audio){
    var constraints = {};
    if (!requested || typeof requested !== 'object') { return constraints; }
    if (requested.advanced !== undefined) { throw new TypeError('Display capture does not support advanced constraints'); }
    (audio ? Object.keys(requested) : ['width', 'height', 'frameRate', 'aspectRatio', 'resizeMode', 'displaySurface', 'cursor', 'logicalSurface']).forEach(function(key){
        var value = requested[key];
        if (value && typeof value === 'object') {
            if (value.min !== undefined || value.exact !== undefined) {
                throw new TypeError('Display capture does not support min or exact constraints');
            }
            value = Object.assign({}, value);
            if (Array.isArray(value.ideal)) { value.ideal = value.ideal.slice(); }
        }
        if (value !== undefined) { constraints[key] = value; }
    });
    return constraints;
};
var captureDesktop = function(source, video){
    if (typeof mediaDevices.getUserMedia !== 'function') {
        return Promise.reject(new DOMException('Window capture is unavailable.', 'NotSupportedError'));
    }
    return mediaDevices.getUserMedia({audio: false, video: {mandatory: {
        chromeMediaSource: 'desktop', chromeMediaSourceId: source + ':' + delegatedSourceId() + ':0'
    }}}).then(function(stream){
        return Promise.all(stream.getVideoTracks().map(function(track){
            return Promise.resolve().then(function(){ return track.applyConstraints(video); });
        })).then(function(){ return stream; }, function(error){
            stream.getTracks().forEach(function(track){ track.stop(); });
            throw error;
        });
    });
};
var getDisplayMedia = function(options){
    var requestOptions;
    var video;
    try {
        requestOptions = options && typeof options === 'object' ? Object.assign({}, options) : {};
        if (requestOptions.video === false) { throw new TypeError('Display capture requires video'); }
        video = snapshotConstraints(requestOptions.video);
        if (requestOptions.video && typeof requestOptions.video === 'object') { requestOptions.video = video; }
        if (requestOptions.audio && typeof requestOptions.audio === 'object') {
            requestOptions.audio = snapshotConstraints(requestOptions.audio, true);
        }
        if (!navigator.userActivation || !navigator.userActivation.isActive || !document.hasFocus()) {
            throw new DOMException('Display capture requires an active user gesture in a focused document.', 'InvalidStateError');
        }
        var policy = document.permissionsPolicy || document.featurePolicy;
        if (policy && !policy.allowsFeature('display-capture')) {
            throw new DOMException('Display capture is blocked by this document policy.', 'NotAllowedError');
        }
    } catch (error) { return Promise.reject(error); }
    return requestDisplayCapture(Boolean(requestOptions.audio)).then(function(choice){
        var source = choice.source;
        if (source !== 'window' && source !== 'screen') {
            throw new DOMException('The display capture request was cancelled.', 'NotAllowedError');
        }
        return Promise.resolve().then(function(){
            if (source === 'screen' && !portalCapture) { return nativeGetDisplayMedia(requestOptions); }
            return captureDesktop(source, video);
        }).finally(function(){ finishDisplayCapture(choice.id); });
    }, function(){
        throw new DOMException('The display capture request was cancelled.', 'NotAllowedError');
    });
};
Object.defineProperty(mediaDevices, 'getDisplayMedia', {
    configurable: true,
    enumerable: false,
    value: getDisplayMedia,
    writable: true
});
})();)JS";

    }

    CefRefPtr<CefRenderProcessHandler> CefRendererApp::GetRenderProcessHandler() {
        return this;
    }

    void
    CefRendererApp::OnFocusedNodeChanged(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefDOMNode> node) {
        if (!frame) {
            return;
        }
        const bool editable = node && node->IsEditable();
        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_text_input_state");
        CefRefPtr<CefListValue> values = message->GetArgumentList();
        values->SetBool(0, editable);
        values->SetBool(
            1,
            editable && node->IsFormControlElement() &&
                node->GetFormControlElementType() == DOM_FORM_CONTROL_TYPE_INPUT_PASSWORD
        );
        frame->SendProcessMessage(PID_BROWSER, message);
    }

    void CefRendererApp::OnContextCreated(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context
    ) {
        if (!frame || !context) {
            return;
        }
        if (frame->IsMain() && context->IsSame(frame->GetV8Context())) {
            const std::string frameId = frame->GetIdentifier().ToString();
            const CefRefPtr<CefV8Value> origin = context->GetGlobal()->GetValue("origin");
            m_autofillDocuments[frameId] = {
                context,
                frameId + ":" + std::to_string(++m_nextAutofillDocumentId),
                origin && origin->IsString() ? origin->GetStringValue().ToString() : std::string()
            };
        }
        CefRefPtr<CefCommandLine> commandLine = CefCommandLine::GetGlobalCommandLine();
        const std::string clientId =
            commandLine ? commandLine->GetSwitchValue("renderer-client-id").ToString() : std::string();
        if (frame->IsMain() && !clientId.empty()) {
            CefRefPtr<CefProcessMessage> clientMessage = CefProcessMessage::Create("eden_renderer_client_id");
            clientMessage->GetArgumentList()->SetInt(0, std::atoi(clientId.c_str()));
            frame->SendProcessMessage(PID_BROWSER, clientMessage);
        }
        const bool mainWorld = context->IsSame(frame->GetV8Context());
        if (mainWorld) {
            context->GetGlobal()->SetValue(
                "__edenReportCopy",
                CefV8Value::CreateFunction(
                    "reportCopy",
                    new ClipboardReportHandler(
                        frame->GetIdentifier().ToString() + ":" + std::to_string(++m_nextClipboardDocumentId)
                    )
                ),
                V8_PROPERTY_ATTRIBUTE_DONTENUM
            );
        }
        CefRefPtr<FormReportHandler> formHandler = new FormReportHandler();
        context->GetGlobal()->SetValue(
            "__edenReportCredential",
            CefV8Value::CreateFunction("reportCredential", formHandler),
            V8_PROPERTY_ATTRIBUTE_DONTENUM
        );
        context->GetGlobal()->SetValue(
            "__edenReportFormField",
            CefV8Value::CreateFunction("reportFormField", formHandler),
            V8_PROPERTY_ATTRIBUTE_DONTENUM
        );
        context->GetGlobal()->SetValue(
            "__edenFinishDisplayCapture",
            CefV8Value::CreateFunction("finishDisplayCapture", new DisplayCaptureRequestHandler(this)),
            V8_PROPERTY_ATTRIBUTE_DONTENUM
        );
        context->GetGlobal()->SetValue(
            "__edenRequestDisplayCapture",
            CefV8Value::CreateFunction("requestDisplayCapture", new DisplayCaptureRequestHandler(this)),
            V8_PROPERTY_ATTRIBUTE_DONTENUM
        );
        if (mainWorld) {
            frame->ExecuteJavaScript(kClipboardHookScript, frame->GetURL(), 0);
        }
        frame->ExecuteJavaScript(kFormHookScript, frame->GetURL(), 0);
        if (mainWorld) {
            const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
            context->GetGlobal()->SetValue(
                "__edenPortalCapture",
                CefV8Value::CreateBool(waylandDisplay && *waylandDisplay),
                V8_PROPERTY_ATTRIBUTE_DONTENUM
            );
            frame->ExecuteJavaScript(kDisplayCaptureHookScript, frame->GetURL(), 0);
            context->GetGlobal()->SetValue(
                "__edenMediaActivity",
                CefV8Value::CreateFunction("mediaActivity", new MediaActivityHandler()),
                V8_PROPERTY_ATTRIBUTE_DONTENUM
            );
            const std::string script =
                std::string("(function(){const report=window.__edenMediaActivity;delete window.__edenMediaActivity;") +
                mediaActivityScript + "(report);})();";
            frame->ExecuteJavaScript(script, frame->GetURL(), 0);
        }
    }

    void CefRendererApp::OnContextReleased(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context
    ) {
        if (frame) {
            const auto document = m_autofillDocuments.find(frame->GetIdentifier().ToString());
            if (document != m_autofillDocuments.end() && document->second.context->IsSame(context)) {
                m_autofillDocuments.erase(document);
            }
        }
        for (auto iterator = m_displayCapturePromises.begin(); iterator != m_displayCapturePromises.end();) {
            if (iterator->second.context && iterator->second.context->IsSame(context)) {
                iterator = m_displayCapturePromises.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    bool CefRendererApp::OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId sourceProcess,
        CefRefPtr<CefProcessMessage> message
    ) {
        if (browser && frame && message && sourceProcess == PID_BROWSER && message->GetName() == "eden_gc_sample") {
            const CefRefPtr<CefListValue> arguments = message->GetArgumentList();
            if (!frame->IsMain() || arguments->GetSize() != 1 || arguments->GetType(0) != VTYPE_STRING) {
                return true;
            }
            const CefRefPtr<CefProcessMessage> response = CefProcessMessage::Create("eden_gc_sample");
            const CefRefPtr<CefListValue> values = response->GetArgumentList();
            values->SetString(0, arguments->GetString(0));
            const auto document = m_autofillDocuments.find(frame->GetIdentifier().ToString());
            const CefRefPtr<CefV8Context> context = frame->GetV8Context();
            timespec cpu{};
            timespec wall{};
            if (document != m_autofillDocuments.end() && context && context->IsValid() &&
                document->second.context->IsSame(context) && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0 &&
                clock_gettime(CLOCK_MONOTONIC, &wall) == 0) {
                values->SetString(1, document->second.id);
                values->SetDouble(2, double(cpu.tv_sec) + double(cpu.tv_nsec) / 1000000000.0);
                values->SetDouble(3, double(wall.tv_sec) + double(wall.tv_nsec) / 1000000000.0);
            }
            frame->SendProcessMessage(PID_BROWSER, response);
            return true;
        }
        if (browser && frame && message && sourceProcess == PID_BROWSER &&
            (message->GetName() == "eden_get_autofill_target" || message->GetName() == "eden_fill_credential" ||
             message->GetName() == "eden_fill_form")) {
            const auto document = m_autofillDocuments.find(frame->GetIdentifier().ToString());
            const CefRefPtr<CefV8Context> current = frame->GetV8Context();
            const bool valid = frame->IsMain() && document != m_autofillDocuments.end() && current &&
                               current->IsValid() && document->second.context->IsSame(current) &&
                               !document->second.origin.empty() && document->second.origin != "null";
            const CefRefPtr<CefListValue> arguments = message->GetArgumentList();
            if (message->GetName() == "eden_get_autofill_target") {
                const CefRefPtr<CefProcessMessage> response = CefProcessMessage::Create("eden_autofill_target");
                const CefRefPtr<CefListValue> values = response->GetArgumentList();
                values->SetString(0, arguments->GetString(0));
                if (valid) {
                    values->SetString(1, document->second.id);
                    values->SetString(2, document->second.origin);
                    values->SetString(3, frame->GetURL());
                }
                frame->SendProcessMessage(PID_BROWSER, response);
            } else if (
                valid && arguments->GetSize() == 3 && arguments->GetString(0) == document->second.id &&
                arguments->GetString(1) == document->second.origin && current->Enter()
            ) {
                CefRefPtr<CefV8Value> result;
                CefRefPtr<CefV8Exception> exception;
                current->Eval(arguments->GetString(2), frame->GetURL(), 0, result, exception);
                current->Exit();
            }
            return true;
        }
        if (!browser || !message || message->GetName() != "eden_display_capture_response") {
            return false;
        }
        CefRefPtr<CefListValue> arguments = message->GetArgumentList();
        const int requestId = arguments->GetInt(0);
        const std::string source = arguments->GetString(1).ToString();
        const auto found = m_displayCapturePromises.find({browser->GetIdentifier(), requestId});
        if (found == m_displayCapturePromises.end()) {
            return true;
        }
        DisplayCapturePromise pending = found->second;
        m_displayCapturePromises.erase(found);
        if (!pending.context || !pending.promise || !pending.context->Enter()) {
            return true;
        }
        if (source == "window" || source == "screen") {
            const CefRefPtr<CefV8Value> choice = CefV8Value::CreateObject(nullptr, nullptr);
            choice->SetValue("source", CefV8Value::CreateString(source), V8_PROPERTY_ATTRIBUTE_NONE);
            choice->SetValue("id", CefV8Value::CreateInt(requestId), V8_PROPERTY_ATTRIBUTE_NONE);
            pending.promise->ResolvePromise(choice);
        } else {
            pending.promise->RejectPromise("The display capture request was cancelled");
        }
        pending.context->Exit();
        return true;
    }

    bool CefRendererApp::requestDisplayCapture(
        CefRefPtr<CefV8Context> context,
        bool audioRequested,
        CefRefPtr<CefV8Value> &returnValue,
        CefString &exception
    ) {
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        CefRefPtr<CefBrowser> browser = context ? context->GetBrowser() : nullptr;
        if (!frame || !browser) {
            exception = "Display capture is unavailable";
            return true;
        }
        CefRefPtr<CefV8Value> promise = CefV8Value::CreatePromise();
        if (!promise) {
            exception = "Display capture is unavailable";
            return true;
        }
        const int requestId = m_nextDisplayCaptureRequestId++;
        m_displayCapturePromises.emplace(
            std::make_pair(browser->GetIdentifier(), requestId),
            DisplayCapturePromise{context, promise}
        );
        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_display_capture_request");
        message->GetArgumentList()->SetInt(0, requestId);
        message->GetArgumentList()->SetBool(1, audioRequested);
        frame->SendProcessMessage(PID_BROWSER, message);
        returnValue = promise;
        return true;
    }

}
