from wpe.patch_support import replace_once


WEBGL_CONTEXT = "Source/WebCore/html/canvas/WebGLRenderingContextBase.cpp"


def webgl_context_patch(text):
    return replace_once(
        text,
        "void WebGLRenderingContextBase::stop()\n"
        "{\n"
        "    if (!isContextLost()) {\n"
        "        forceLostContext(SyntheticLostContext);\n"
        "        destroyGraphicsContextGL();\n"
        "    }\n"
        "}\n",
        "void WebGLRenderingContextBase::stop()\n"
        "{\n"
        "    if (isContextLost())\n"
        "        return;\n"
        "    m_contextLostState = ContextLostState { SyntheticLostContext };\n"
        "    m_contextLostState->errors.add(GCGLErrorCode::ContextLost);\n"
        "    {\n"
        "        Locker locker { objectGraphLock() };\n"
        "        detachAndRemoveAllObjects();\n"
        "    }\n"
        "    loseExtensions(SyntheticLostContext);\n"
        "    destroyGraphicsContextGL();\n"
        "}\n",
    )


PATCHES = {
    WEBGL_CONTEXT: webgl_context_patch,
}
