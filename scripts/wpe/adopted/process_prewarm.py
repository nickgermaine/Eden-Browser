from wpe.patch_support import replace_once


API = "Source/WebKit/UIProcess/API/glib/"
POOL = "Source/WebKit/UIProcess/"
PROXY = "Source/WebKit/UIProcess/WebProcessProxy.h"


def context_header_patch(text):
    return replace_once(text, "G_END_DECLS", """#if ENABLE(2022_GLIB_API)
WEBKIT_API void
webkit_web_context_set_automatic_process_warming_enabled(WebKitWebContext*, gboolean);

WEBKIT_API gboolean
webkit_web_context_get_automatic_process_warming_enabled(WebKitWebContext*);

WEBKIT_API gboolean
webkit_web_context_prewarm_web_process(WebKitWebContext*, WebKitNetworkSession*);

WEBKIT_API guint64
webkit_web_context_get_prewarmed_web_process_identifier(WebKitWebContext*);
#endif

G_END_DECLS""")


def context_patch(text):
    text = replace_once(text, "    configuration->setUsesWebProcessCache(true);",
                        "    configuration->setUsesWebProcessCache(true);\n"
                        "    configuration->setIsAutomaticProcessWarmingEnabled(true);")
    return text + """
#if ENABLE(2022_GLIB_API)
void webkit_web_context_set_automatic_process_warming_enabled(WebKitWebContext* context, gboolean enabled)
{
    g_return_if_fail(WEBKIT_IS_WEB_CONTEXT(context));
    auto& pool = *context->priv->processPool;
    pool.configuration().setIsAutomaticProcessWarmingEnabled(enabled);
    if (!enabled) {
        pool.clearPrewarmedProcesses();
    }
}

gboolean webkit_web_context_get_automatic_process_warming_enabled(WebKitWebContext* context)
{
    g_return_val_if_fail(WEBKIT_IS_WEB_CONTEXT(context), FALSE);
    return context->priv->processPool->configuration().isAutomaticProcessWarmingEnabled();
}

gboolean webkit_web_context_prewarm_web_process(WebKitWebContext* context, WebKitNetworkSession* session)
{
    g_return_val_if_fail(WEBKIT_IS_WEB_CONTEXT(context), FALSE);
    g_return_val_if_fail(WEBKIT_IS_NETWORK_SESSION(session), FALSE);
    auto& store = webkitWebsiteDataManagerGetDataStore(webkit_network_session_get_website_data_manager(session));
    return context->priv->processPool->prewarmProcessForDataStore(store);
}

guint64 webkit_web_context_get_prewarmed_web_process_identifier(WebKitWebContext* context)
{
    g_return_val_if_fail(WEBKIT_IS_WEB_CONTEXT(context), 0);
    auto identifiers = context->priv->processPool->prewarmedProcessIdentifiers();
    if (identifiers.isEmpty()) {
        return 0;
    }
    return *identifiers.begin();
}
#endif
"""


def pool_header_patch(text):
    text = replace_once(text, "    void prewarmProcess();", """    void prewarmProcess();
    bool prewarmProcessForDataStore(WebsiteDataStore&);
    void clearPrewarmedProcesses();""")
    text = replace_once(text, "    WeakHashSet<WebProcessProxy> m_prewarmedProcesses;",
                        "    WeakHashSet<WebProcessProxy> m_prewarmedProcesses;\n"
                        "    RefPtr<WebsiteDataStore> m_prewarmingDataStore;")
    return replace_once(text, "    unsigned prewarmedProcessCountLimit() const;",
                        "    unsigned prewarmedProcessCountLimit() const;\n"
                        "    unsigned livePrewarmedProcessCount() const;")


def proxy_header_patch(text):
    return replace_once(text, "    void shutDown();",
                        "    void shutDown();\n"
                        "    bool isShuttingDown() const { return m_isShuttingDown; }")


def pool_patch(text):
    text = replace_once(text, """    for (Ref process : m_prewarmedProcesses) {
        if (process->lockdownMode() != lockdownMode || process->enhancedSecurity() != enhancedSecurity)""",
                        """    for (Ref process : m_prewarmedProcesses) {
        if (m_prewarmingDataStore && process->websiteDataStore() != &websiteDataStore) {
            continue;
        }
#if PLATFORM(GTK) || PLATFORM(WPE)
        if (m_sandboxEnabled && !m_prewarmingDataStore) {
            continue;
        }
#endif
        if (process->lockdownMode() != lockdownMode || process->enhancedSecurity() != enhancedSecurity)""")
    start = text.index("#if PLATFORM(GTK) || PLATFORM(WPE)", text.index("bool didRemove = m_prewarmedProcesses.remove(*prewarmedProcess)"))
    end = text.index("#endif", start) + len("#endif")
    text = text[:start] + text[end:]
    text = replace_once(text, "    prewarmedProcess->setWebsiteDataStore(websiteDataStore);", """    if (!m_prewarmingDataStore) {
        prewarmedProcess->setWebsiteDataStore(websiteDataStore);
    }""")
    text = replace_once(text, "void WebProcessPool::prewarmProcess()\n{", """bool WebProcessPool::prewarmProcessForDataStore(WebsiteDataStore& dataStore)
{
    if (m_prewarmingDataStore && m_prewarmingDataStore.get() != &dataStore) {
        return false;
    }
    if (!configuration().isAutomaticProcessWarmingEnabled()
        || !configuration().processSwapsOnNavigation() || usesSingleWebProcess()) {
        return false;
    }
    if (!m_prewarmingDataStore) {
        clearPrewarmedProcesses();
        m_prewarmingDataStore = &dataStore;
    }
    for (WeakPtr maybeProcess : copyToVector(m_prewarmedProcesses)) {
        if (RefPtr process = maybeProcess.get(); process && !process->isShuttingDown() && process->wasTerminated()) {
            process->shutDown();
        }
    }
    prewarmProcess();
    return livePrewarmedProcessCount() > 0;
}

void WebProcessPool::clearPrewarmedProcesses()
{
    for (WeakPtr maybeProcess : copyToVector(m_prewarmedProcesses)) {
        if (RefPtr process = maybeProcess.get()) {
            process->shutDown();
        }
    }
}

void WebProcessPool::prewarmProcess()
{""")
    text = replace_once(text, """void WebProcessPool::prewarmProcess()
{
    if (m_prewarmedProcesses.computeSize() >= prewarmedProcessCountLimit())
        return;
""", """void WebProcessPool::prewarmProcess()
{
    if (MemoryPressureHandler::singleton().isUnderMemoryPressure()) {
        return;
    }
    if (m_prewarmingDataStore && !configuration().isAutomaticProcessWarmingEnabled()) {
        return;
    }
#if PLATFORM(GTK) || PLATFORM(WPE)
    if (m_sandboxEnabled && !m_prewarmingDataStore) {
        return;
    }
#endif
    if (m_prewarmedProcesses.computeSize() >= prewarmedProcessCountLimit())
        return;
""")
    text = replace_once(text, "if (m_prewarmedProcesses.computeSize() >= prewarmedProcessCountLimit())",
                        "if (livePrewarmedProcessCount() >= prewarmedProcessCountLimit())")
    text = replace_once(text, "if (process.isPrewarmed() && m_prewarmedProcesses.computeSize() < prewarmedProcessCountLimit())",
                        "if (process.isPrewarmed() && livePrewarmedProcessCount() < prewarmedProcessCountLimit())")
    text = replace_once(text, "    createNewWebProcess(nullptr, lockdownMode, enhancedSecurity, WebProcessProxy::EnableWebAssemblyDebugger::No, WebProcessProxy::IsPrewarmed::Yes);",
                        "    createNewWebProcess(m_prewarmingDataStore.get(), lockdownMode, enhancedSecurity, WebProcessProxy::EnableWebAssemblyDebugger::No, WebProcessProxy::IsPrewarmed::Yes);")
    text = replace_once(text, "unsigned WebProcessPool::prewarmedProcessCountLimit() const\n{", """unsigned WebProcessPool::livePrewarmedProcessCount() const
{
    unsigned count = 0;
    for (Ref process : m_prewarmedProcesses) {
        if (!process->isShuttingDown()
            && process->state() != AuxiliaryProcessProxy::State::Terminated && !process->wasTerminated()) {
            ++count;
        }
    }
    return count;
}

unsigned WebProcessPool::prewarmedProcessCountLimit() const
{
    if (m_prewarmingDataStore) {
        return 1;
    }""")
    text = replace_once(text, "        if (auto pid = process->processID())\n            pids.add(pid);", """        if (process->isShuttingDown() || process->state() != AuxiliaryProcessProxy::State::Running || process->wasTerminated())
            continue;
        if (auto pid = process->processID())
            pids.add(pid);""")
    return text


def launch_patch(text):
    return replace_once(text, "        RefPtr dataStore = isPrewarmed() ? &WebsiteDataStore::defaultDataStore() : websiteDataStore();",
                        """        RefPtr dataStore = websiteDataStore();
        if (!dataStore && isPrewarmed()) {
            dataStore = &WebsiteDataStore::defaultDataStore();
        }""")


PATCHES = {
    API + "WebKitWebContext.h.in": context_header_patch,
    API + "WebKitWebContext.cpp": context_patch,
    POOL + "WebProcessPool.h": pool_header_patch,
    POOL + "WebProcessPool.cpp": pool_patch,
    PROXY: proxy_header_patch,
    POOL + "glib/WebProcessProxyGLib.cpp": launch_patch,
}
