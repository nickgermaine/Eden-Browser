#pragma once

#include <WebCore/ExceptionOr.h>
#include <WebCore/HTMLElement.h>
#include <WebCore/HTMLNames.h>
#include <WebCore/ShadowRoot.h>
#include <WebCore/ShadowRootInit.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

namespace TestWebKitAPI {

    class EdenRemovalAggregateElement final : public WebCore::HTMLElement {
        WTF_MAKE_TZONE_ALLOCATED_INLINE(EdenRemovalAggregateElement);
        WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(EdenRemovalAggregateElement);

      public:
        static WTF::Ref<EdenRemovalAggregateElement> create(
            WebCore::Document &document,
            const WebCore::QualifiedName &tagName,
            unsigned &removalStepsCount,
            unsigned &destructionCount
        ) {
            return WTF::adoptRef(
                *new EdenRemovalAggregateElement(document, tagName, removalStepsCount, destructionCount)
            );
        }

        ~EdenRemovalAggregateElement() final {
            ++m_destructionCount;
        }

      protected:
        void
        removingSteps(WebCore::Node::RemovalType removalType, WebCore::ContainerNode &oldParentOfRemovedTree) final {
            WebCore::HTMLElement::removingSteps(removalType, oldParentOfRemovedTree);
            ++m_removalStepsCount;
        }

      private:
        EdenRemovalAggregateElement(
            WebCore::Document &document,
            const WebCore::QualifiedName &tagName,
            unsigned &removalStepsCount,
            unsigned &destructionCount
        )
            : WebCore::HTMLElement(tagName, document),
              m_removalStepsCount(removalStepsCount),
              m_destructionCount(destructionCount) {}

        unsigned &m_removalStepsCount;
        unsigned &m_destructionCount;
    };

    struct EdenRemovalAggregateTree {
        WTF::RefPtr<EdenRemovalAggregateElement> root;
        WTF::Vector<EdenRemovalAggregateElement *> elements;
        WebCore::ShadowRoot *shadowRoot{nullptr};
    };

    inline EdenRemovalAggregateTree edenMakeRemovalAggregateTree(
        WebCore::Document &document,
        unsigned &removalStepsCount,
        unsigned &destructionCount,
        bool largeShadowChildFirst,
        bool includeLargeShadowChild = true
    ) {
        auto root = EdenRemovalAggregateElement::create(
            document,
            WebCore::HTMLNames::divTag,
            removalStepsCount,
            destructionCount
        );
        WTF::Vector<EdenRemovalAggregateElement *> elements;
        elements.append(root.ptr());

        WebCore::ShadowRootInit shadowRootInit;
        shadowRootInit.mode = WebCore::ShadowRootMode::Open;
        auto shadowRootResult = root->attachShadow(shadowRootInit);
        RELEASE_ASSERT(!shadowRootResult.hasException());
        auto *shadowRoot = &shadowRootResult.releaseReturnValue();

        auto lightFirst = EdenRemovalAggregateElement::create(
            document,
            WebCore::HTMLNames::spanTag,
            removalStepsCount,
            destructionCount
        );
        elements.append(lightFirst.ptr());
        auto firstAppend = root->appendChild(lightFirst.get());
        RELEASE_ASSERT(!firstAppend.hasException());

        auto lightMiddle = EdenRemovalAggregateElement::create(
            document,
            WebCore::HTMLNames::spanTag,
            removalStepsCount,
            destructionCount
        );
        elements.append(lightMiddle.ptr());
        auto middleAppend = root->appendChild(lightMiddle.get());
        RELEASE_ASSERT(!middleAppend.hasException());

        auto lightNested = EdenRemovalAggregateElement::create(
            document,
            WebCore::HTMLNames::spanTag,
            removalStepsCount,
            destructionCount
        );
        elements.append(lightNested.ptr());
        auto nestedAppend = lightMiddle->appendChild(lightNested.get());
        RELEASE_ASSERT(!nestedAppend.hasException());

        auto lightLast = EdenRemovalAggregateElement::create(
            document,
            WebCore::HTMLNames::spanTag,
            removalStepsCount,
            destructionCount
        );
        elements.append(lightLast.ptr());
        auto lastAppend = root->appendChild(lightLast.get());
        RELEASE_ASSERT(!lastAppend.hasException());

        auto shadowFirst = EdenRemovalAggregateElement::create(
            document,
            includeLargeShadowChild && largeShadowChildFirst ? WebCore::HTMLNames::imgTag : WebCore::HTMLNames::spanTag,
            removalStepsCount,
            destructionCount
        );
        elements.append(shadowFirst.ptr());
        auto shadowFirstAppend = shadowRoot->appendChild(shadowFirst.get());
        RELEASE_ASSERT(!shadowFirstAppend.hasException());

        auto shadowLast = EdenRemovalAggregateElement::create(
            document,
            includeLargeShadowChild && !largeShadowChildFirst ? WebCore::HTMLNames::imgTag
                                                              : WebCore::HTMLNames::spanTag,
            removalStepsCount,
            destructionCount
        );
        elements.append(shadowLast.ptr());
        auto shadowLastAppend = shadowRoot->appendChild(shadowLast.get());
        RELEASE_ASSERT(!shadowLastAppend.hasException());

        return {WTF::move(root), WTF::move(elements), shadowRoot};
    }

}
