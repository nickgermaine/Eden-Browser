#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <Qt>

namespace eden::core {

class WindowFrame : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)

  public:
    explicit WindowFrame(QObject *parent = nullptr);

    QQuickWindow *window() const;
    void setWindow(QQuickWindow *window);
    Q_INVOKABLE bool startSystemMove();
    Q_INVOKABLE bool startSystemResize(int edges);
    Q_INVOKABLE void toggleMaximized();
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void close();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  signals:
    void windowChanged();

  private:
    QPointer<QQuickWindow> m_window;
};

}
