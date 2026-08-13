#include "engine/cef/cefrendererapp.h"

#include <cstdlib>

#include "include/cef_command_line.h"
#include "include/cef_v8.h"

namespace eden::engine::cef {

namespace {

class ClipboardReportHandler final : public CefV8Handler {
  public:
    bool Execute(const CefString &name, CefRefPtr<CefV8Value>, const CefV8ValueList &arguments, CefRefPtr<CefV8Value> &,
                 CefString &) override {
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
})();)JS";

}

CefRefPtr<CefRenderProcessHandler> CefRendererApp::GetRenderProcessHandler() {
    return this;
}

void CefRendererApp::OnContextCreated(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) {
    if (!frame || !context) {
        return;
    }
    CefRefPtr<CefCommandLine> commandLine = CefCommandLine::GetGlobalCommandLine();
    const std::string clientId = commandLine ? commandLine->GetSwitchValue("renderer-client-id").ToString() : std::string();
    if (!clientId.empty()) {
        CefRefPtr<CefProcessMessage> clientMessage = CefProcessMessage::Create("eden_renderer_client_id");
        clientMessage->GetArgumentList()->SetInt(0, std::atoi(clientId.c_str()));
        frame->SendProcessMessage(PID_BROWSER, clientMessage);
    }
    context->GetGlobal()->SetValue("__edenReportCopy", CefV8Value::CreateFunction("reportCopy", new ClipboardReportHandler()),
                                   V8_PROPERTY_ATTRIBUTE_DONTENUM);
    frame->ExecuteJavaScript(kClipboardHookScript, frame->GetURL(), 0);
}

}
