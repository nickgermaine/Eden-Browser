#include "engine/cef/cefrendererapp.h"

#include <cstdlib>

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

        class ClipboardReportHandler final : public CefV8Handler {
          public:
            bool Execute(
                const CefString &name,
                CefRefPtr<CefV8Value>,
                const CefV8ValueList &arguments,
                CefRefPtr<CefV8Value> &,
                CefString &
            ) override {
                if (name != "reportCopy" || arguments.size() != 1 || !arguments[0]->IsString()) {
                    return false;
                }
                CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
                CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
                if (!frame) {
                    return true;
                }
                CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_clipboard_copy");
                message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
                frame->SendProcessMessage(PID_BROWSER, message);
                return true;
            }

          private:
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
var report = window.__edenReportCopy;
delete window.__edenReportCopy;
if (!report) { return; }
var forward = function(text){ if (text) { try { report(String(text)); } catch (error) {} } };
var grab = function(event){
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
        navigator.clipboard.writeText = function(value){ forward(value); return writeText(value); };
    }
    if (navigator.clipboard.write) {
        var write = navigator.clipboard.write.bind(navigator.clipboard);
        navigator.clipboard.write = function(items){
            try {
                Array.prototype.forEach.call(items || [], function(item){
                    if (item && item.types && item.getType && item.types.indexOf('text/plain') >= 0) {
                        item.getType('text/plain').then(function(blob){ return blob.text(); }).then(forward).catch(function(){});
                    }
                });
            } catch (error) {}
            return write(items);
        };
    }
}
try {
    var frontendHost = window.InspectorFrontendHost;
    if (frontendHost && typeof frontendHost.copyText === 'function') {
        var copyText = frontendHost.copyText.bind(frontendHost);
        frontendHost.copyText = function(value){ forward(value); return copyText(value); };
    }
} catch (error) {}
})();)JS";

        constexpr const char *kFormHookScript = R"JS((function(){
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
    return visible(field) && field.value &&
        (autocompleteHas(field, 'username') || autocompleteHas(field, 'email') || field.type === 'email' ||
         /user|email|login|identifier/.test(fieldIdentity(field)));
};
var secretField = function(field){
    if (!visible(field) || !field.value || autocompleteHas(field, 'one-time-code')) { return false; }
    if (field.type === 'password' || autocompleteHas(field, 'current-password') || autocompleteHas(field, 'new-password')) {
        return true;
    }
    return /password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-]*key/.test(fieldIdentity(field));
};
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
document.addEventListener('submit', function(event){ capture(event.target); }, true);
document.addEventListener('click', function(event){
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
    var field = event.target;
    if (field && /^(INPUT|TEXTAREA)$/.test(field.tagName) && usernameField(field)) {
        try { reportCredential(field.value || '', ''); } catch (error) {}
    }
}, true);
document.addEventListener('keydown', function(event){
    if (event.key === 'Enter') { capture(event.target.form || document); }
}, true);
var reportField = function(field){
    if (!field || !/^(INPUT|TEXTAREA)$/.test(field.tagName)) { return; }
    var rect = field.getBoundingClientRect();
    var value = secretField(field) ? '' : field.value || '';
    try {
        reportFormField(
            field.type || 'text',
            field.name || field.id || '',
            field.autocomplete || '',
            value,
            rect.x,
            rect.y,
            rect.width,
            rect.height
        );
    } catch (error) {}
};
document.addEventListener('focusin', function(event){
    reportField(event.target);
}, true);
document.addEventListener('input', function(event){
    reportField(event.target);
}, true);
})();)JS";

        constexpr const char *kDisplayCaptureHookScript = R"JS((function(){
var requestDisplayCapture = window.__edenRequestDisplayCapture;
delete window.__edenRequestDisplayCapture;
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
var windowConstraints = function(requested){
    var constraints = requested && typeof requested === 'object' ? Object.assign({}, requested) : {};
    constraints.mandatory = Object.assign({}, constraints.mandatory || {}, {
        chromeMediaSource: 'desktop',
        chromeMediaSourceId: 'window:' + delegatedSourceId() + ':0'
    });
    delete constraints.displaySurface;
    delete constraints.monitorTypeSurfaces;
    delete constraints.preferCurrentTab;
    delete constraints.selfBrowserSurface;
    delete constraints.surfaceSwitching;
    delete constraints.systemAudio;
    return constraints;
};
var captureWindow = function(options){
    if (typeof mediaDevices.getUserMedia !== 'function') {
        return Promise.reject(new DOMException('Window capture is unavailable.', 'NotSupportedError'));
    }
    var video = options.video === undefined ? true : options.video;
    if (video === false) {
        return Promise.reject(new TypeError('Display capture requires video'));
    }
    return mediaDevices.getUserMedia({
        audio: false,
        video: windowConstraints(video)
    });
};
var getDisplayMedia = function(options){
    var requestOptions = options && typeof options === 'object' ? options : {};
    var audioRequested = Boolean(requestOptions.audio);
    return requestDisplayCapture(audioRequested).then(function(source){
        if (source !== 'window' && source !== 'screen') {
            throw new DOMException('The display capture request was cancelled.', 'NotAllowedError');
        }
        return source === 'window' ? captureWindow(requestOptions) : nativeGetDisplayMedia(requestOptions);
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

    void CefRendererApp::OnContextCreated(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context
    ) {
        if (!frame || !context) {
            return;
        }
        CefRefPtr<CefCommandLine> commandLine = CefCommandLine::GetGlobalCommandLine();
        const std::string clientId =
            commandLine ? commandLine->GetSwitchValue("renderer-client-id").ToString() : std::string();
        if (!clientId.empty()) {
            CefRefPtr<CefProcessMessage> clientMessage = CefProcessMessage::Create("eden_renderer_client_id");
            clientMessage->GetArgumentList()->SetInt(0, std::atoi(clientId.c_str()));
            frame->SendProcessMessage(PID_BROWSER, clientMessage);
        }
        context->GetGlobal()->SetValue(
            "__edenReportCopy",
            CefV8Value::CreateFunction("reportCopy", new ClipboardReportHandler()),
            V8_PROPERTY_ATTRIBUTE_DONTENUM
        );
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
            "__edenRequestDisplayCapture",
            CefV8Value::CreateFunction("requestDisplayCapture", new DisplayCaptureRequestHandler(this)),
            V8_PROPERTY_ATTRIBUTE_DONTENUM
        );
        frame->ExecuteJavaScript(kClipboardHookScript, frame->GetURL(), 0);
        frame->ExecuteJavaScript(kFormHookScript, frame->GetURL(), 0);
        frame->ExecuteJavaScript(kDisplayCaptureHookScript, frame->GetURL(), 0);
    }

    void
    CefRendererApp::OnContextReleased(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefV8Context> context) {
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
        CefRefPtr<CefFrame>,
        CefProcessId,
        CefRefPtr<CefProcessMessage> message
    ) {
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
            pending.promise->ResolvePromise(CefV8Value::CreateString(source));
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
