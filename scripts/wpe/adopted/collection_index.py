from wpe.patch_support import replace_once


IDL = "Source/WebCore/html/HTMLCollection.idl"
CUSTOM_SOURCE = "Source/WebCore/bindings/js/JSHTMLCollectionCustom.cpp"
SOURCES_LIST = "Source/WebCore/Sources.txt"


def html_collection_idl_patch(text):
    if "CustomGetOwnPropertySlot" in text:
        raise RuntimeError("The HTMLCollection custom property-slot attribute is already present")
    return replace_once(
        text,
        "    GenerateIsReachable=ImplOwnerNodeRoot,\n",
        "    CustomGetOwnPropertySlot,\n    CustomPut,\n    GenerateIsReachable=ImplOwnerNodeRoot,\n",
    )


ORIGINAL_INCLUDES = (
    '#include "HTMLCollectionInlines.h"\n'
    '#include "JSDOMBinding.h"\n'
    '#include "JSHTMLAllCollection.h"\n'
    '#include "JSHTMLFormControlsCollection.h"\n'
    '#include "JSHTMLOptionsCollection.h"\n'
)

CUSTOM_INCLUDES = (
    '#include "DOMWrapperWorld.h"\n'
    '#include "Document.h"\n'
    '#include "DocumentQuirks.h"\n'
    '#include "Element.h"\n'
    '#include "HTMLCollectionInlines.h"\n'
    '#include "JSDOMAbstractOperations.h"\n'
    '#include "JSDOMBinding.h"\n'
    '#include "JSDOMConvertInterface.h"\n'
    '#include "JSDOMConvertNullable.h"\n'
    '#include "JSDOMExceptionHandling.h"\n'
    '#include "JSDOMGlobalObject.h"\n'
    '#include "JSElement.h"\n'
    '#include "ScriptExecutionContext.h"\n'
    '#include <JavaScriptCore/IndexingType.h>\n'
)

GENERATED_WRAPPER_FACTORIES = """JSValue toJSNewlyCreated(JSGlobalObject*, JSDOMGlobalObject* globalObject, Ref<HTMLCollection>&& collection)
{
    switch (collection->type()) {
    case CollectionType::FormControls:
        return createWrapper<HTMLFormControlsCollection>(globalObject, WTF::move(collection));
    case CollectionType::SelectOptions:
        return createWrapper<HTMLOptionsCollection>(globalObject, WTF::move(collection));
    case CollectionType::DocAll:
        return createWrapper<HTMLAllCollection>(globalObject, WTF::move(collection));
    default:
        break;
    }

    return createWrapper<HTMLCollection>(globalObject, WTF::move(collection));
}

JSValue toJS(JSGlobalObject* lexicalGlobalObject, JSDOMGlobalObject* globalObject, HTMLCollection& collection)
{
    return wrap(lexicalGlobalObject, globalObject, collection);
}

"""

PROPERTY_METHODS = '''static bool getHTMLCollectionOwnProperty(JSObject* object, JSGlobalObject* lexicalGlobalObject, PropertyName propertyName, PropertySlot& slot, bool ignoreNamedProperties)
{
    auto throwScope = DECLARE_THROW_SCOPE(JSC::getVM(lexicalGlobalObject));
    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(object);
    ASSERT_GC_OBJECT_INHERITS(thisObject, JSHTMLCollection::info());
    if (auto index = parseIndex(propertyName)) {
        if (auto item = thisObject->wrapped().item(index.value()); !!item) [[likely]] {
            auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, IDLNullable<IDLInterface<Element>>::extractValueFromNullable(WTF::move(item)));
            RETURN_IF_EXCEPTION(throwScope, false);
            slot.setValue(thisObject, static_cast<unsigned>(JSC::PropertyAttribute::ReadOnly), value);
            return true;
        }
        if (!hasIndexedProperties(thisObject->indexingType()))
            return false;
        return WebCore::getOwnPropertySlot(object, lexicalGlobalObject, propertyName, slot);
    }
    if (!ignoreNamedProperties) {
        using GetterIDLType = IDLInterface<Element>;
        auto getterFunctor = visibleNamedPropertyItemAccessorFunctor<GetterIDLType, JSHTMLCollection>([] (JSHTMLCollection& thisObject, PropertyName propertyName) -> decltype(auto) {
            return thisObject.wrapped().namedItem(propertyNameToAtomString(propertyName));
        });
        if (auto namedProperty = accessVisibleNamedProperty<LegacyOverrideBuiltIns::No>(*lexicalGlobalObject, *thisObject, propertyName, getterFunctor)) {
            auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, WTF::move(namedProperty.value()));
            RETURN_IF_EXCEPTION(throwScope, false);
            slot.setValue(thisObject, JSC::PropertyAttribute::ReadOnly | JSC::PropertyAttribute::DontEnum, value);
            return true;
        }
    }
    return WebCore::getOwnPropertySlot(object, lexicalGlobalObject, propertyName, slot);
}

bool JSHTMLCollection::getOwnPropertySlot(JSObject* object, JSGlobalObject* lexicalGlobalObject, PropertyName propertyName, PropertySlot& slot)
{
    bool ignoreNamedProperties = false;
    return getHTMLCollectionOwnProperty(object, lexicalGlobalObject, propertyName, slot, ignoreNamedProperties);
}

bool JSHTMLCollection::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* lexicalGlobalObject, unsigned index, PropertySlot& slot)
{
    SUPPRESS_UNCOUNTED_LOCAL auto& vm = JSC::getVM(lexicalGlobalObject);
    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(object);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    if (index <= MAX_ARRAY_INDEX) [[likely]] {
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        if (auto item = thisObject->wrapped().item(index); !!item) [[likely]] {
            auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, IDLNullable<IDLInterface<Element>>::extractValueFromNullable(WTF::move(item)));
            RETURN_IF_EXCEPTION(throwScope, false);
            slot.setValue(thisObject, static_cast<unsigned>(JSC::PropertyAttribute::ReadOnly), value);
            return true;
        }
        if (!hasIndexedProperties(thisObject->indexingType()))
            return false;
        return JSObject::getOwnPropertySlotByIndex(object, lexicalGlobalObject, index, slot);
    }
    auto propertyName = Identifier::from(vm, index);
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    using GetterIDLType = IDLInterface<Element>;
    auto getterFunctor = visibleNamedPropertyItemAccessorFunctor<GetterIDLType, JSHTMLCollection>([] (JSHTMLCollection& thisObject, PropertyName propertyName) -> decltype(auto) {
        return thisObject.wrapped().namedItem(propertyNameToAtomString(propertyName));
    });
    if (auto namedProperty = accessVisibleNamedProperty<LegacyOverrideBuiltIns::No>(*lexicalGlobalObject, *thisObject, propertyName, getterFunctor)) {
        auto value = toJS<IDLNullable<IDLInterface<Element>>>(*lexicalGlobalObject, *thisObject->realm(), throwScope, WTF::move(namedProperty.value()));
        RETURN_IF_EXCEPTION(throwScope, false);
        slot.setValue(thisObject, JSC::PropertyAttribute::ReadOnly | JSC::PropertyAttribute::DontEnum, value);
        return true;
    }
    return JSObject::getOwnPropertySlotByIndex(object, lexicalGlobalObject, index, slot);
}

bool JSHTMLCollection::put(JSCell* cell, JSGlobalObject* lexicalGlobalObject, PropertyName propertyName, JSValue value, PutPropertySlot& putPropertySlot)
{
    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    if (thisObject != putPropertySlot.thisValue()) [[unlikely]]
        return JSObject::put(thisObject, lexicalGlobalObject, propertyName, value, putPropertySlot);

    if (auto* document = dynamicDowncast<Document>(dynamicDowncast<JSDOMGlobalObject>(lexicalGlobalObject)->scriptExecutionContext())) {
        if (document->quirks().needsConfigurableIndexedPropertiesQuirk()) [[unlikely]]
            return JSObject::put(thisObject, lexicalGlobalObject, propertyName, value, putPropertySlot);
    }

    auto throwScope = DECLARE_THROW_SCOPE(lexicalGlobalObject->vm());

    throwScope.assertNoException();
    PropertyDescriptor ownDescriptor;
    PropertySlot slot(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
    bool ignoreNamedProperties = true;
    bool hasOwnProperty = getHTMLCollectionOwnProperty(thisObject, lexicalGlobalObject, propertyName, slot, ignoreNamedProperties);
    RETURN_IF_EXCEPTION(throwScope, false);
    if (hasOwnProperty) {
        ownDescriptor.setPropertySlot(lexicalGlobalObject, propertyName, slot);
        RETURN_IF_EXCEPTION(throwScope, false);
    }
    RELEASE_AND_RETURN(throwScope, ordinarySetWithOwnDescriptor(lexicalGlobalObject, thisObject, propertyName, value, putPropertySlot.thisValue(), WTF::move(ownDescriptor), putPropertySlot.isStrictMode()));
}

bool JSHTMLCollection::putByIndex(JSCell* cell, JSGlobalObject* lexicalGlobalObject, unsigned index, JSValue value, bool shouldThrow)
{
    if (auto* document = dynamicDowncast<Document>(dynamicDowncast<JSDOMGlobalObject>(lexicalGlobalObject)->scriptExecutionContext())) {
        if (document->quirks().needsConfigurableIndexedPropertiesQuirk()) [[unlikely]]
            return JSObject::putByIndex(cell, lexicalGlobalObject, index, value, shouldThrow);
    }

    auto* thisObject = uncheckedDowncast<JSHTMLCollection>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    SUPPRESS_UNCOUNTED_LOCAL auto& vm = JSC::getVM(lexicalGlobalObject);
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    throwScope.assertNoException();
    auto propertyName = Identifier::from(vm, index);
    PutPropertySlot putPropertySlot(thisObject, shouldThrow);
    RELEASE_AND_RETURN(throwScope, ordinarySetSlow(lexicalGlobalObject, thisObject, propertyName, value, putPropertySlot.thisValue(), shouldThrow));
}

'''


def html_collection_custom_source_patch(text):
    if "bool JSHTMLCollection::getOwnPropertySlot(" in text:
        raise RuntimeError("The HTMLCollection custom property-slot implementation is already present")
    text = replace_once(text, ORIGINAL_INCLUDES, CUSTOM_INCLUDES)
    return replace_once(text, GENERATED_WRAPPER_FACTORIES, PROPERTY_METHODS)


def sources_list_patch(text):
    if "bindings/js/JSHTMLCollectionCustom.cpp\n" in text:
        raise RuntimeError("The HTMLCollection custom binding source is already listed")
    return replace_once(
        text,
        "bindings/js/JSHTMLAllCollectionCustom.cpp\n",
        "bindings/js/JSHTMLAllCollectionCustom.cpp\nbindings/js/JSHTMLCollectionCustom.cpp\n",
    )


PATCHES = {
    IDL: html_collection_idl_patch,
    CUSTOM_SOURCE: html_collection_custom_source_patch,
    SOURCES_LIST: sources_list_patch,
}

FILES = {
    "LayoutTests/fast/dom/eden-html-collection-indexed-getter.html": "collection_index_test.html",
    "LayoutTests/fast/dom/eden-html-collection-indexed-getter-expected.txt": "collection_index_test_expected.txt",
    "LayoutTests/resources/eden-html-collection-indexed-getter.js": "collection_index_test.js",
}

SOURCES = [
    "collection_index_test.html",
    "collection_index_test.js",
    "collection_index_test_expected.txt",
]
