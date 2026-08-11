#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>

namespace eden::core {

class TabStripNavigator : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickItem *view READ view WRITE setView NOTIFY viewChanged)

  public:
    explicit TabStripNavigator(QObject *parent = nullptr);

    QQuickItem *view() const;
    void setView(QQuickItem *view);
    Q_INVOKABLE void slide(qreal distance);
    Q_INVOKABLE void snap(int direction);
    Q_INVOKABLE void reveal(int index);
    Q_INVOKABLE int destinationForDrag(int sourceIndex, qreal centerX) const;

  signals:
    void viewChanged();

  private:
    enum PositionMode { Beginning = 0, End = 2, Contain = 4 };

    int indexAt(qreal x, qreal y) const;
    void positionViewAtIndex(int index, PositionMode mode);

    QPointer<QQuickItem> m_view;
};

}
