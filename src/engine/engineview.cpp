#include "engine/engineview.h"

namespace eden::engine {

EngineView::EngineView(QObject *parent)
    : QObject(parent) {}

EngineView::~EngineView() = default;

void EngineView::executeContextMenuCommand(const QString &) {}

}
