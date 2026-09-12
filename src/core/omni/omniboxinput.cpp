#include "core/omni/omniboxinput.h"
#include "core/omni/omniboxcontroller.h"

#include <QEvent>
#include <QKeyEvent>
#include <QScopedValueRollback>

#include <algorithm>

namespace eden::core {

    OmniboxInput::OmniboxInput(OmniboxController *controller)
        : QObject(controller),
          m_controller(controller) {
        connect(controller, &QAbstractItemModel::modelAboutToBeReset, this, &OmniboxInput::suggestionsChanging);
        connect(controller, &QAbstractItemModel::modelReset, this, &OmniboxInput::suggestionsChanged);
    }

    QQuickItem *OmniboxInput::input() const {
        return m_input;
    }

    void OmniboxInput::setInput(QQuickItem *input) {
        if (m_input == input) {
            return;
        }
        if (m_input) {
            m_input->removeEventFilter(this);
            disconnect(m_input, nullptr, this, nullptr);
        }
        m_input = input;
        if (input) {
            input->installEventFilter(this);
            connect(input, SIGNAL(textEdited()), this, SLOT(textEdited()));
            connect(input, SIGNAL(cursorPositionChanged()), this, SLOT(selectionChanged()));
            connect(input, SIGNAL(selectionStartChanged()), this, SLOT(selectionChanged()));
            connect(input, SIGNAL(selectionEndChanged()), this, SLOT(selectionChanged()));
            connect(input, SIGNAL(inputMethodComposingChanged()), this, SLOT(composingChanged()));
            connect(input, &QObject::destroyed, this, [this] {
                m_input = nullptr;
                finish({});
                emit inputChanged();
            });
        }
        emit inputChanged();
    }

    int OmniboxInput::selectedIndex() const {
        return m_selectedIndex;
    }

    bool OmniboxInput::popupOpen() const {
        return m_editing && m_popupOpen && m_controller->rowCount() > 0;
    }

    QString OmniboxInput::text() const {
        return m_input ? m_input->property("text").toString() : QString();
    }

    bool OmniboxInput::hasCompletion() const {
        return m_input && !m_inlineText.isEmpty() && text() == m_inlineText &&
               m_input->property("selectionStart").toInt() == m_controller->query().size() &&
               m_input->property("selectionEnd").toInt() == m_inlineText.size();
    }

    void OmniboxInput::render(const QString &value, int start, int end) {
        if (!m_input) {
            return;
        }
        QScopedValueRollback guard(m_rendering, true);
        if (text() != value) {
            m_input->setProperty("text", value);
        }
        QMetaObject::invokeMethod(m_input, "select", Q_ARG(int, start), Q_ARG(int, end));
    }

    void OmniboxInput::begin(const QString &url) {
        m_editPending = false;
        m_editing = true;
        m_popupOpen = false;
        m_explicitSelection = false;
        m_inlineText.clear();
        m_suppressNextCompletion = true;
        m_controller->updateQuery({}, false);
        const QString value = url == "eden://newtab" || url == "about:blank" ? QString() : url;
        render(value, 0, value.size());
        m_edits.clear();
        m_editIndex = -1;
        rememberEdit();
        emit stateChanged();
    }

    void OmniboxInput::finish(const QString &url) {
        m_editPending = false;
        m_editing = false;
        m_popupOpen = false;
        m_explicitSelection = false;
        m_inlineText.clear();
        m_controller->updateQuery({}, false);
        render(url, 0, 0);
        m_edits.clear();
        m_editIndex = -1;
        emit stateChanged();
    }

    void OmniboxInput::dismiss() {
        if (!m_popupOpen) {
            return;
        }
        m_popupOpen = false;
        if (m_explicitSelection) {
            const QString query = m_controller->query();
            render(query, query.size(), query.size());
        }
        m_explicitSelection = false;
        suppressCompletion();
        emit stateChanged();
    }

    void OmniboxInput::rememberEdit() {
        if (!m_input) {
            return;
        }
        const Edit edit{
            text(),
            m_input->property("cursorPosition").toInt(),
            m_input->property("selectionStart").toInt(),
            m_input->property("selectionEnd").toInt()
        };
        if (m_editIndex >= 0 && m_edits.at(m_editIndex).text == edit.text) {
            m_edits[m_editIndex] = edit;
            return;
        }
        m_edits.resize(m_editIndex + 1);
        m_edits.append(edit);
        if (m_edits.size() > 100) {
            m_edits.removeFirst();
        }
        m_editIndex = m_edits.size() - 1;
    }

    void OmniboxInput::restoreEdit(int delta) {
        const int index = m_editIndex + delta;
        if (index < 0 || index >= m_edits.size()) {
            return;
        }
        m_editIndex = index;
        const Edit edit = m_edits.at(index);
        m_inlineText.clear();
        m_explicitSelection = false;
        m_popupOpen = true;
        {
            QScopedValueRollback guard(m_updatingQuery, true);
            m_controller->updateQuery(edit.text, false);
        }
        render(edit.text, edit.cursor == edit.start ? edit.end : edit.start, edit.cursor);
        m_selectedIndex = m_controller->rowCount() > 0 ? 0 : -1;
        emit stateChanged();
    }

    void OmniboxInput::textEdited() {
        if (m_rendering || !m_editing || !m_input) {
            return;
        }
        m_pendingCompletion = !m_suppressNextCompletion;
        m_suppressNextCompletion = true;
        m_inlineText.clear();
        if (!m_editPending) {
            m_editPending = true;
            QMetaObject::invokeMethod(this, &OmniboxInput::applyEdit, Qt::QueuedConnection);
        }
    }

    void OmniboxInput::applyEdit() {
        if (!m_editPending || !m_editing || !m_input) {
            return;
        }
        m_editPending = false;
        const QString value = text();
        const bool composing = m_input->property("inputMethodComposing").toBool();
        const bool allowCompletion = m_pendingCompletion && !composing &&
                                     m_input->property("cursorPosition").toInt() == value.size() &&
                                     m_input->property("selectionStart") == m_input->property("selectionEnd");
        m_suppressNextCompletion = true;
        m_inlineText.clear();
        m_explicitSelection = false;
        m_popupOpen = !composing;
        rememberEdit();
        {
            QScopedValueRollback guard(m_updatingQuery, true);
            m_controller->updateQuery(value, allowCompletion);
        }
        m_selectedIndex = m_controller->rowCount() > 0 ? 0 : -1;
        if (allowCompletion) {
            showDefault();
        }
        emit stateChanged();
    }

    void OmniboxInput::selectionChanged() {
        if (m_rendering || m_editPending || !m_editing || !m_input || hasCompletion()) {
            return;
        }
        const bool changedCompletion = !m_inlineText.isEmpty() && text() == m_inlineText;
        const bool movedWithinQuery = text() == m_controller->query() &&
                                      (m_input->property("cursorPosition").toInt() != text().size() ||
                                       m_input->property("selectionStart") != m_input->property("selectionEnd"));
        if (!changedCompletion && !movedWithinQuery) {
            return;
        }
        m_inlineText.clear();
        m_explicitSelection = false;
        QScopedValueRollback guard(m_updatingQuery, true);
        m_controller->updateQuery(text(), false);
        rememberEdit();
    }

    void OmniboxInput::composingChanged() {
        if (!m_rendering && m_editing && m_input->property("inputMethodComposing").toBool()) {
            m_popupOpen = false;
            m_inlineText.clear();
            m_controller->updateQuery({}, false);
            emit stateChanged();
        }
    }

    void OmniboxInput::showDefault() {
        if (!m_editing || !m_popupOpen || !m_input || m_input->property("inputMethodComposing").toBool()) {
            return;
        }
        const QString query = m_controller->query();
        const QString suffix = m_controller->completionSuffix();
        const QString value = query + suffix;
        m_inlineText = suffix.isEmpty() ? QString() : value;
        render(value, value.size(), query.size());
    }

    void OmniboxInput::suppressCompletion() {
        const bool completed = hasCompletion();
        const QString query = m_controller->query();
        m_inlineText.clear();
        m_suppressNextCompletion = true;
        {
            QScopedValueRollback guard(m_updatingQuery, true);
            m_controller->updateQuery(query, false);
        }
        if (completed) {
            render(query, query.size(), query.size());
        }
        m_selectedIndex = m_controller->rowCount() > 0 ? 0 : -1;
        emit stateChanged();
    }

    void OmniboxInput::suggestionsChanging() {
        if (m_explicitSelection && m_selectedIndex >= 0) {
            m_selectedUrl = m_controller->suggestionUrl(m_selectedIndex);
            m_selectedKind =
                m_controller->data(m_controller->index(m_selectedIndex), OmniboxController::KindRole).toString();
        }
    }

    void OmniboxInput::suggestionsChanged() {
        if (m_updatingQuery) {
            return;
        }
        m_selectedIndex = m_controller->rowCount() > 0 ? 0 : -1;
        if (m_explicitSelection) {
            for (int row = 0; row < m_controller->rowCount(); ++row) {
                if (m_controller->suggestionUrl(row) == m_selectedUrl &&
                    m_controller->data(m_controller->index(row), OmniboxController::KindRole).toString() ==
                        m_selectedKind) {
                    m_selectedIndex = row;
                    emit stateChanged();
                    return;
                }
            }
            m_explicitSelection = false;
            showDefault();
            emit stateChanged();
            return;
        }
        const bool atQueryEnd = m_input && text() == m_controller->query() &&
                                m_input->property("cursorPosition").toInt() == text().size() &&
                                m_input->property("selectionStart") == m_input->property("selectionEnd");
        if (m_input && ((!m_inlineText.isEmpty() && text() == m_inlineText) ||
                        (atQueryEnd && !m_controller->completionSuffix().isEmpty()))) {
            showDefault();
        }
        emit stateChanged();
    }

    void OmniboxInput::moveSelection(int delta) {
        if (m_controller->rowCount() == 0) {
            m_controller->updateQuery(text(), false);
        }
        if (m_controller->rowCount() == 0) {
            return;
        }
        const int row = m_popupOpen ? std::clamp(m_selectedIndex + delta, 0, m_controller->rowCount() - 1) : 0;
        m_popupOpen = true;
        m_selectedIndex = row;
        m_explicitSelection = row != 0;
        m_inlineText.clear();
        if (row == 0) {
            showDefault();
        } else {
            const QString value = m_controller->suggestionText(row);
            render(value, value.size(), value.size());
        }
        emit stateChanged();
    }

    void OmniboxInput::activate(int row) {
        if (row < 0 || row >= m_controller->rowCount()) {
            return;
        }
        const QUrl url = m_controller->suggestionUrl(row);
        const int tab = m_controller->suggestionTabIndex(row);
        m_popupOpen = false;
        if (tab >= 0) {
            emit tabRequested(tab);
        } else if (url.isValid() && !url.isEmpty()) {
            emit navigationRequested(url);
        }
        emit editingFinished();
    }

    void OmniboxInput::accept(bool controlEnter) {
        if (!controlEnter && m_popupOpen && (m_explicitSelection || hasCompletion())) {
            activate(m_selectedIndex);
            return;
        }
        const QString value = controlEnter && hasCompletion() ? m_controller->query() : text();
        const QUrl url = m_controller->destination(value, controlEnter);
        if (!url.isValid() || url.isEmpty()) {
            return;
        }
        m_popupOpen = false;
        emit navigationRequested(url);
        emit editingFinished();
    }

    bool OmniboxInput::eventFilter(QObject *watched, QEvent *event) {
        if (watched != m_input || !m_editing || m_rendering) {
            return false;
        }
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::InputMethod) {
            applyEdit();
        }
        if (event->type() == QEvent::InputMethod) {
            suppressCompletion();
            m_suppressNextCompletion = true;
            return false;
        }
        if (event->type() != QEvent::KeyPress) {
            return false;
        }
        auto *key = static_cast<QKeyEvent *>(event);
        if (m_input->property("inputMethodComposing").toBool()) {
            m_suppressNextCompletion = true;
            return false;
        }
        if (key->matches(QKeySequence::Undo)) {
            restoreEdit(-1);
            return true;
        }
        if (key->matches(QKeySequence::Redo)) {
            restoreEdit(1);
            return true;
        }
        const bool plain = key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::KeypadModifier;
        if (plain && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            moveSelection(key->key() == Qt::Key_Down ? 1 : -1);
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            accept(key->modifiers().testFlag(Qt::ControlModifier));
            return true;
        }
        if (plain && key->key() == Qt::Key_Escape) {
            if (popupOpen()) {
                render(m_controller->query(), m_controller->query().size(), m_controller->query().size());
                dismiss();
            } else {
                emit editingFinished();
            }
            return true;
        }
        if (plain && hasCompletion() && (key->key() == Qt::Key_Right || key->key() == Qt::Key_End)) {
            const QString value = m_controller->suggestionUrl(0).toDisplayString(QUrl::RemoveUserInfo);
            m_inlineText.clear();
            m_explicitSelection = false;
            {
                QScopedValueRollback guard(m_updatingQuery, true);
                m_controller->updateQuery(value, false);
            }
            render(value, value.size(), value.size());
            rememberEdit();
            return true;
        }
        if (plain && hasCompletion() &&
            (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete || key->key() == Qt::Key_Left)) {
            suppressCompletion();
            return true;
        }
        m_suppressNextCompletion = key->text().isEmpty() || key->matches(QKeySequence::Paste) ||
                                   key->matches(QKeySequence::Cut) || key->key() == Qt::Key_Backspace ||
                                   key->key() == Qt::Key_Delete || key->modifiers().testFlag(Qt::ControlModifier) ||
                                   key->modifiers().testFlag(Qt::AltModifier) ||
                                   key->modifiers().testFlag(Qt::MetaModifier);
        return false;
    }

}
