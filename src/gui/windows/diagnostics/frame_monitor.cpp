#include "diagnostics/frame_monitor.hpp"

#include <QQuickWindow>
#include <QSGRendererInterface>

namespace ccs_trans::gui {

FrameMonitor::FrameMonitor(QObject* parent) : QObject(parent) {}

void FrameMonitor::attach(QQuickWindow* window) {
    window_ = window;
    connect(window_, &QQuickWindow::frameSwapped, this, [this] { ++frameCount_; });
}

std::uint64_t FrameMonitor::frameCount() const noexcept {
    return frameCount_;
}

QString FrameMonitor::graphicsApiName() const {
    if (window_ == nullptr || window_->rendererInterface() == nullptr) {
        return QStringLiteral("unknown");
    }
    switch (window_->rendererInterface()->graphicsApi()) {
        case QSGRendererInterface::Software:
            return QStringLiteral("software");
        case QSGRendererInterface::OpenGL:
            return QStringLiteral("opengl");
        case QSGRendererInterface::Direct3D11:
            return QStringLiteral("direct3d11");
        case QSGRendererInterface::Direct3D12:
            return QStringLiteral("direct3d12");
        case QSGRendererInterface::Vulkan:
            return QStringLiteral("vulkan");
        case QSGRendererInterface::Metal:
            return QStringLiteral("metal");
        case QSGRendererInterface::Null:
            return QStringLiteral("null");
        default:
            return QStringLiteral("unknown");
    }
}

}  // namespace ccs_trans::gui
