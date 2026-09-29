from wpe.patch_support import replace_once


API = "Source/WebKit/UIProcess/API/glib/"


def header_patch(text):
    return replace_once(text, "G_END_DECLS", """WEBKIT_API void
webkit_web_view_preconnect_to_uri(WebKitWebView*, const gchar*);

G_END_DECLS""")


def view_patch(text):
    text = replace_once(text, '#include "APIData.h"',
                        '#include "APIData.h"\n#include <WebCore/ResourceRequest.h>')
    return text + """
void webkit_web_view_preconnect_to_uri(WebKitWebView* view, const gchar* uri)
{
    g_return_if_fail(WEBKIT_IS_WEB_VIEW(view));
    g_return_if_fail(uri);
    URL url({ }, String::fromUTF8(uri));
    if (!url.isValid() || !url.protocolIs("https"_s) || url.host().isEmpty() || !url.user().isEmpty() || !url.password().isEmpty()) {
        return;
    }
    url.setPath("/"_s);
    url.removeQueryAndFragmentIdentifier();
    getPage(view).preconnectTo(WebCore::ResourceRequest { WTF::move(url) });
}
"""


PATCHES = {
    API + "WebKitWebView.h.in": header_patch,
    API + "WebKitWebView.cpp": view_patch,
}
