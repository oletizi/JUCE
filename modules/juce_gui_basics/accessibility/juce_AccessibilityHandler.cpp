/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 6 End-User License
   Agreement and JUCE Privacy Policy (both effective as of the 16th June 2020).

   End User License Agreement: www.juce.com/juce-6-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

AccessibilityHandler* AccessibilityHandler::currentlyFocusedHandler = nullptr;

enum class InternalAccessibilityEvent
{
    elementCreated,
    elementDestroyed,
    focusChanged,
    windowOpened,
    windowClosed
};

void notifyAccessibilityEventInternal (const AccessibilityHandler& handler, InternalAccessibilityEvent event);

inline String getAccessibleApplicationOrPluginName()
{
   #if defined (JucePlugin_Name)
    return JucePlugin_Name;
   #else
    if (auto* app = JUCEApplicationBase::getInstance())
        return app->getApplicationName();

    return "JUCE Application";
   #endif
}

AccessibilityHandler::AccessibilityHandler (Component& comp,
                                            AccessibilityRole accessibilityRole,
                                            AccessibilityActions accessibilityActions,
                                            Interfaces interfacesIn)
    : component (comp),
      typeIndex (typeid (component)),
      role (accessibilityRole),
      actions (std::move (accessibilityActions)),
      interfaces (std::move (interfacesIn)),
      nativeImpl (createNativeImpl (*this))
{
    notifyAccessibilityEventInternal (*this, InternalAccessibilityEvent::elementCreated);
}

AccessibilityHandler::~AccessibilityHandler()
{
    giveAwayFocus();
    notifyAccessibilityEventInternal (*this, InternalAccessibilityEvent::elementDestroyed);
}

//==============================================================================
AccessibleState AccessibilityHandler::getCurrentState() const
{
    auto state = AccessibleState().withFocusable();

    return hasFocus (false) ? state.withFocused() : state;
}

bool AccessibilityHandler::isIgnored() const
{
    return role == AccessibilityRole::ignored
        || getCurrentState().isIgnored()
        || component.isCurrentlyBlockedByAnotherModalComponent();
}

static bool isComponentVisibleWithinWindow (const Component& comp)
{
    if (auto* peer = comp.getPeer())
        return ! peer->getAreaCoveredBy (comp).getIntersection (peer->getComponent().getLocalBounds()).isEmpty();

    return false;
}

static bool isComponentVisibleWithinParent (Component* comp)
{
    if (auto* parent = comp->getParentComponent())
    {
        if (comp->getBoundsInParent().getIntersection (parent->getLocalBounds()).isEmpty())
            return false;

        return isComponentVisibleWithinParent (parent);
    }

    return true;
}

bool AccessibilityHandler::isVisibleWithinParent() const
{
    return getCurrentState().isAccessibleOffscreen()
          || (isComponentVisibleWithinParent (&component) && isComponentVisibleWithinWindow (component));
}

//==============================================================================
const AccessibilityActions& AccessibilityHandler::getActions() const noexcept
{
    return actions;
}

AccessibilityValueInterface* AccessibilityHandler::getValueInterface() const
{
    return interfaces.value.get();
}

AccessibilityTableInterface* AccessibilityHandler::getTableInterface() const
{
    return interfaces.table.get();
}

AccessibilityCellInterface* AccessibilityHandler::getCellInterface() const
{
    return interfaces.cell.get();
}

AccessibilityTextInterface* AccessibilityHandler::getTextInterface() const
{
    return interfaces.text.get();
}

//==============================================================================
static AccessibilityHandler* findEnclosingHandler (Component* comp)
{
    if (comp != nullptr)
    {
        if (auto* handler = comp->getAccessibilityHandler())
            return handler;

        return findEnclosingHandler (comp->getParentComponent());
    }

    return nullptr;
}

static AccessibilityHandler* getUnignoredAncestor (AccessibilityHandler* handler)
{
    while (handler != nullptr
           && (handler->isIgnored() || ! handler->isVisibleWithinParent())
           && handler->getParent() != nullptr)
    {
        handler = handler->getParent();
    }

    return handler;
}

static AccessibilityHandler* findFirstUnignoredChild (const std::vector<AccessibilityHandler*>& handlers)
{
    if (! handlers.empty())
    {
        const auto iter = std::find_if (handlers.cbegin(), handlers.cend(),
                                        [] (const AccessibilityHandler* handler) { return ! handler->isIgnored() && handler->isVisibleWithinParent(); });

        if (iter != handlers.cend())
            return *iter;

        for (auto* handler : handlers)
            if (auto* unignored = findFirstUnignoredChild (handler->getChildren()))
                return unignored;
    }

    return nullptr;
}

static AccessibilityHandler* getFirstUnignoredDescendant (AccessibilityHandler* handler)
{
    if (handler != nullptr && (handler->isIgnored() || ! handler->isVisibleWithinParent()))
        return findFirstUnignoredChild (handler->getChildren());

    return handler;
}

AccessibilityHandler* AccessibilityHandler::getParent() const
{
    if (auto* focusContainer = component.findFocusContainer())
        return getUnignoredAncestor (findEnclosingHandler (focusContainer));

    return nullptr;
}

std::vector<AccessibilityHandler*> AccessibilityHandler::getChildren() const
{
    if (! component.isFocusContainer() && component.getParentComponent() != nullptr)
        return {};

    std::vector<AccessibilityHandler*> children;

    if (auto traverser = component.createFocusTraverser())
    {
        for (auto* focusableChild : traverser->getAllComponents (&component))
        {
            if (auto* handler = findEnclosingHandler (focusableChild))
            {
                if (! isParentOf (handler))
                    continue;

                if (auto* unignored = getFirstUnignoredDescendant (handler))
                    if (std::find (children.cbegin(), children.cend(), unignored) == children.cend())
                        children.push_back (unignored);
            }
        }
    }

    return children;
}

bool AccessibilityHandler::isParentOf (const AccessibilityHandler* possibleChild) const noexcept
{
    while (possibleChild != nullptr)
    {
        possibleChild = possibleChild->getParent();

        if (possibleChild == this)
            return true;
    }

    return false;
}

AccessibilityHandler* AccessibilityHandler::getChildAt (Point<int> screenPoint)
{
    if (auto* comp = Desktop::getInstance().findComponentAt (screenPoint))
        if (isParentOf (comp->getAccessibilityHandler()))
            return getUnignoredAncestor (findEnclosingHandler (comp));

    return nullptr;
}

AccessibilityHandler* AccessibilityHandler::getChildFocus()
{
    return hasFocus (true) ? getUnignoredAncestor (currentlyFocusedHandler)
                           : nullptr;
}

bool AccessibilityHandler::hasFocus (bool trueIfChildFocused) const
{
    return currentlyFocusedHandler != nullptr
            && (currentlyFocusedHandler == this
                || (trueIfChildFocused && isParentOf (currentlyFocusedHandler)));
}

void AccessibilityHandler::grabFocus()
{
    if (! hasFocus (false))
        grabFocusInternal (true);
}

void AccessibilityHandler::giveAwayFocus() const
{
    if (hasFocus (true))
        giveAwayFocusInternal();
}

void AccessibilityHandler::grabFocusInternal (bool canTryParent)
{
    if (getCurrentState().isFocusable() && ! isIgnored())
    {
        takeFocus();
        return;
    }

    if (isParentOf (currentlyFocusedHandler) && ! currentlyFocusedHandler->isIgnored())
        return;

    if (component.isFocusContainer() || component.getParentComponent() == nullptr)
    {
        if (auto traverser = component.createFocusTraverser())
        {
            if (auto* defaultComp = traverser->getDefaultComponent (&component))
            {
                if (auto* handler = getUnignoredAncestor (findEnclosingHandler (defaultComp)))
                {
                    if (isParentOf (handler))
                    {
                        handler->grabFocusInternal (false);
                        return;
                    }
                }
            }
        }
    }

    if (canTryParent)
        if (auto* parent = getParent())
            parent->grabFocusInternal (true);
}

void AccessibilityHandler::giveAwayFocusInternal() const
{
    currentlyFocusedHandler = nullptr;

    if (auto* parent = getParent())
        notifyAccessibilityEventInternal (*parent, InternalAccessibilityEvent::focusChanged);
}

void AccessibilityHandler::takeFocus()
{
    currentlyFocusedHandler = this;
    notifyAccessibilityEventInternal (*this, InternalAccessibilityEvent::focusChanged);

    if (component.getWantsKeyboardFocus() && ! component.hasKeyboardFocus (true))
        component.grabKeyboardFocus();
}

//==============================================================================
#if ! ((JUCE_MAC && (defined (MAC_OS_X_VERSION_10_10) && MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_10)) \
     || (JUCE_WINDOWS && ! JUCE_MINGW))

class AccessibilityHandler::AccessibilityNativeImpl { public: AccessibilityNativeImpl (AccessibilityHandler&) {} };
void AccessibilityHandler::notifyAccessibilityEvent (AccessibilityEvent) const {}
void AccessibilityHandler::postAnnouncement (const String&, AnnouncementPriority) {}
AccessibilityNativeHandle* AccessibilityHandler::getNativeImplementation() const { return nullptr; }
AccessibilityHandler::AccessibilityNativeImpl* AccessibilityHandler::createNativeImpl (AccessibilityHandler&) { return nullptr; }
void AccessibilityHandler::DestroyNativeImpl::operator() (AccessibilityHandler::AccessibilityNativeImpl*) const noexcept {}
void notifyAccessibilityEventInternal (const AccessibilityHandler&, InternalAccessibilityEvent) {}

#endif

} // namespace juce
