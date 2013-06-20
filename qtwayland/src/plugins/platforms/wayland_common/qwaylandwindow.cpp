/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the config.tests of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwaylandwindow.h"

#include "qwaylandbuffer.h"
#include "qwaylanddisplay.h"
#include "qwaylandinputdevice.h"
#include "qwaylandscreen.h"
#include "qwaylandshellsurface.h"
#include "qwaylandextendedsurface.h"
#include "qwaylandsubsurface.h"
#include "qwaylanddecoration.h"
#include "qwaylandwindowmanagerintegration.h"

#include <QtCore/QFileInfo>
#include <QtGui/QWindow>

#include <QCoreApplication>
#include <qpa/qwindowsysteminterface.h>

#include <QtCore/QDebug>

QT_USE_NAMESPACE

QWaylandWindow::QWaylandWindow(QWindow *window)
    : QObject()
    , QPlatformWindow(window)
    , mDisplay(QWaylandScreen::waylandScreenFromWindow(window)->display())
    , mShellSurface(0)
    , mExtendedWindow(0)
    , mSubSurfaceWindow(0)
    , mWindowDecoration(0)
    , mMouseEventsInContentArea(false)
    , mMousePressedInContentArea(Qt::NoButton)
    , mBuffer(0)
    , mWaitingForFrameSync(false)
    , mFrameCallback(0)
    , mResizeExposedSent(false)
    , mSentInitialResize(false)
    , mState(Qt::WindowNoState)
{
    init(mDisplay->createSurface(static_cast<QtWayland::wl_surface *>(this)));

    static WId id = 1;
    mWindowId = id++;

    if (mDisplay->shell() && !(window->flags() & Qt::BypassWindowManagerHint))
        mShellSurface = new QWaylandShellSurface(mDisplay->shell()->get_shell_surface(wl_surface()), this);
    if (mDisplay->windowExtension())
        mExtendedWindow = new QWaylandExtendedSurface(this, mDisplay->windowExtension()->get_extended_surface(wl_surface()));
    if (mDisplay->subSurfaceExtension())
        mSubSurfaceWindow = new QWaylandSubSurface(this, mDisplay->subSurfaceExtension()->get_sub_surface_aware_surface(wl_surface()));

    if (mShellSurface) {
        // Set surface class to the .desktop file name (obtained from executable name)
        QFileInfo exeFileInfo(qApp->applicationFilePath());
        QString className = exeFileInfo.baseName() + QLatin1String(".desktop");
        mShellSurface->setClassName(className.toUtf8().constData());
    }

    if (QPlatformWindow::parent() && mSubSurfaceWindow) {
        mSubSurfaceWindow->setParent(static_cast<const QWaylandWindow *>(QPlatformWindow::parent()));
    } else if (window->transientParent()) {
        if (window->transientParent() && mShellSurface)
            mShellSurface->updateTransientParent(window->transientParent());
    } else if (mShellSurface) {
        mShellSurface->setTopLevel();
    }

    setWindowFlags(window->flags());
    setGeometry(window->geometry());

}

QWaylandWindow::~QWaylandWindow()
{
    if (isInitialized()) {
        delete mShellSurface;
        delete mExtendedWindow;
        destroy();
    }

    QList<QWaylandInputDevice *> inputDevices = mDisplay->inputDevices();
    for (int i = 0; i < inputDevices.size(); ++i)
        inputDevices.at(i)->handleWindowDestroyed(this);
}

QWaylandWindow *QWaylandWindow::fromWlSurface(::wl_surface *surface)
{
    return static_cast<QWaylandWindow *>(static_cast<QtWayland::wl_surface *>(wl_surface_get_user_data(surface)));
}

WId QWaylandWindow::winId() const
{
    return mWindowId;
}

void QWaylandWindow::setParent(const QPlatformWindow *parent)
{
    const QWaylandWindow *parentWaylandWindow = static_cast<const QWaylandWindow *>(parent);
    if (subSurfaceWindow()) {
        subSurfaceWindow()->setParent(parentWaylandWindow);
    }
}

void QWaylandWindow::setWindowTitle(const QString &title)
{
    if (mShellSurface) {
        QByteArray titleUtf8 = title.toUtf8();
        mShellSurface->setTitle(titleUtf8.constData());
    }

    if (mWindowDecoration && window()->isVisible())
        mWindowDecoration->update();
}

void QWaylandWindow::setWindowIcon(const QIcon &icon)
{
    mWindowIcon = icon;

    if (mWindowDecoration && window()->isVisible())
        mWindowDecoration->update();
}

void QWaylandWindow::setGeometry(const QRect &rect)
{
    QPlatformWindow::setGeometry(rect);

    if (shellSurface() && window()->transientParent())
        shellSurface()->updateTransientParent(window()->transientParent());

    if (mWindowDecoration && window()->isVisible())
        mWindowDecoration->update();
}

void QWaylandWindow::setVisible(bool visible)
{

    if (visible) {
        if (mBuffer)
            attach(mBuffer->buffer(), 0, 0);

        if (!mSentInitialResize) {
            QWindowSystemInterface::handleGeometryChange(window(), geometry());
            mSentInitialResize = true;
        }

        QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), geometry().size()));
        // Don't flush the events here, or else the newly visible window may start drawing, but since
        // there was no frame before it will be stuck at the waitForFrameSync() in
        // QWaylandShmBackingStore::beginPaint().
    } else {
        QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), geometry().size()));
        attach(static_cast<QWaylandBuffer *>(0), 0, 0);
    }
    damage(QRect(QPoint(0,0),geometry().size()));
}


bool QWaylandWindow::isExposed() const
{
    if (!window()->isVisible())
        return false;
    if (mExtendedWindow)
        return mExtendedWindow->isExposed();
    return true;
}


void QWaylandWindow::configure(uint32_t edges, int32_t width, int32_t height)
{
    mConfigure.edges |= edges;
    mConfigure.width = width;
    mConfigure.height = height;

    if (!mResizeExposedSent) {
        mResizeExposedSent = true;
        QMetaObject::invokeMethod(this, "doResize", Qt::QueuedConnection);
    }
}

void QWaylandWindow::doResize()
{
    mResizeExposedSent = false;
    if (mConfigure.isEmpty()) {
        return;
    }

    int widthWithoutMargins = qMax(mConfigure.width-(frameMargins().left() +frameMargins().right()),1);
    int heightWithoutMargins = qMax(mConfigure.height-(frameMargins().top()+frameMargins().bottom()),1);

    widthWithoutMargins = qMax(widthWithoutMargins, window()->minimumSize().width());
    heightWithoutMargins = qMax(heightWithoutMargins, window()->minimumSize().height());
    QRect geometry = QRect(0,0, widthWithoutMargins, heightWithoutMargins);

    int x = 0;
    int y = 0;
    QSize size = this->geometry().size();
    if (mConfigure.edges & WL_SHELL_SURFACE_RESIZE_LEFT) {
        x = size.width() - geometry.width();
    }
    if (mConfigure.edges & WL_SHELL_SURFACE_RESIZE_TOP) {
        y = size.height() - geometry.height();
    }
    mOffset += QPoint(x, y);

    mResizeLock.lock();
    setGeometry(geometry);
    mResizeLock.unlock();

    mConfigure.clear();
    QWindowSystemInterface::handleGeometryChange(window(), geometry);
    QWindowSystemInterface::handleExposeEvent(window(), QRegion(geometry));
    QWindowSystemInterface::flushWindowSystemEvents();
}

void QWaylandWindow::attach(QWaylandBuffer *buffer, int x, int y)
{
    mBuffer = buffer;

    if (mBuffer)
        attach(mBuffer->buffer(), x, y);
    else
        QtWayland::wl_surface::attach(0, 0, 0);
}

void QWaylandWindow::attachOffset(QWaylandBuffer *buffer)
{
    attach(buffer, mOffset.x(), mOffset.y());
    mOffset = QPoint();
}

QWaylandBuffer *QWaylandWindow::attached() const
{
    return mBuffer;
}

void QWaylandWindow::damage(const QRect &rect)
{
    //We have to do sync stuff before calling damage, or we might
    //get a frame callback before we get the timestamp
    if (!mWaitingForFrameSync) {
        mFrameCallback = frame();
        wl_callback_add_listener(mFrameCallback,&QWaylandWindow::callbackListener,this);
        mWaitingForFrameSync = true;
    }
    if (mBuffer) {
        damage(rect.x(), rect.y(), rect.width(), rect.height());
    }
    commit();
}

const wl_callback_listener QWaylandWindow::callbackListener = {
    QWaylandWindow::frameCallback
};

void QWaylandWindow::frameCallback(void *data, struct wl_callback *callback, uint32_t time)
{
    Q_UNUSED(time);
    QWaylandWindow *self = static_cast<QWaylandWindow*>(data);
    if (callback != self->mFrameCallback) // might be a callback caused by the shm backingstore
        return;
    self->mWaitingForFrameSync = false;
    if (self->mFrameCallback) {
        wl_callback_destroy(self->mFrameCallback);
        self->mFrameCallback = 0;
    }
}

QMutex QWaylandWindow::mFrameSyncMutex;

void QWaylandWindow::waitForFrameSync()
{
    QMutexLocker locker(&mFrameSyncMutex);
    if (!mWaitingForFrameSync)
        return;
    mDisplay->flushRequests();
    while (mWaitingForFrameSync)
        mDisplay->blockingReadEvents();
}

QMargins QWaylandWindow::frameMargins() const
{
    if (mWindowDecoration)
        return mWindowDecoration->margins();
    return QPlatformWindow::frameMargins();
}

QWaylandShellSurface *QWaylandWindow::shellSurface() const
{
    return mShellSurface;
}

QWaylandExtendedSurface *QWaylandWindow::extendedWindow() const
{
    return mExtendedWindow;
}

QWaylandSubSurface *QWaylandWindow::subSurfaceWindow() const
{
    return mSubSurfaceWindow;
}

void QWaylandWindow::handleContentOrientationChange(Qt::ScreenOrientation orientation)
{
    if (mExtendedWindow)
        mExtendedWindow->setContentOrientation(orientation);
}

void QWaylandWindow::setWindowState(Qt::WindowState state)
{
    if (mState == state) {
        return;
    }

    // As of february 2013 QWindow::setWindowState sets the new state value after
    // QPlatformWindow::setWindowState returns, so we cannot rely on QWindow::windowState
    // here. We use then this mState variable.
    mState = state;
    createDecoration();
    switch (state) {
        case Qt::WindowFullScreen:
            mShellSurface->setFullscreen();
            break;
        case Qt::WindowMaximized:
            mShellSurface->setMaximized();
            break;
        case Qt::WindowMinimized:
            mShellSurface->setMinimized();
            break;
        default:
            mShellSurface->setNormal();
    }
}

void QWaylandWindow::setWindowFlags(Qt::WindowFlags flags)
{
    if (mExtendedWindow)
        mExtendedWindow->setWindowFlags(flags);
}

bool QWaylandWindow::createDecoration()
{
    static bool disableWaylandDecorations = !qgetenv("QT_WAYLAND_DISABLE_WINDOWDECORATION").isEmpty();
    if (disableWaylandDecorations)
        return false;

    bool decoration = false;
    switch (window()->type()) {
        case Qt::Window:
        case Qt::Widget:
        case Qt::Dialog:
        case Qt::Tool:
        case Qt::Drawer:
            decoration = true;
            break;
        default:
            break;
    }
    if (window()->flags() & Qt::FramelessWindowHint || isFullscreen())
        decoration = false;
    if (window()->flags() & Qt::BypassWindowManagerHint)
        decoration = false;

    if (decoration) {
        if (!mWindowDecoration)
            mWindowDecoration = new QWaylandDecoration(this);
    } else {
        delete mWindowDecoration;
        mWindowDecoration = 0;
    }

    return mWindowDecoration;
}

QWaylandDecoration *QWaylandWindow::decoration() const
{
    return mWindowDecoration;
}

void QWaylandWindow::setDecoration(QWaylandDecoration *decoration)
{
    mWindowDecoration = decoration;
    if (subSurfaceWindow()) {
        subSurfaceWindow()->adjustPositionOfChildren();
    }
}

void QWaylandWindow::handleMouse(QWaylandInputDevice *inputDevice, ulong timestamp, const QPointF &local, const QPointF &global, Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    if (mWindowDecoration) {
        handleMouseEventWithDecoration(inputDevice, timestamp,local,global,b,mods);
        return;
    }

    QWindowSystemInterface::handleMouseEvent(window(),timestamp,local,global,b,mods);
}

void QWaylandWindow::handleMouseEnter()
{
    if (!mWindowDecoration) {
        QWindowSystemInterface::handleEnterEvent(window());
    }
}

void QWaylandWindow::handleMouseLeave()
{
    if (mWindowDecoration) {
        if (mMouseEventsInContentArea) {
            QWindowSystemInterface::handleLeaveEvent(window());
        }
        mWindowDecoration->restoreMouseCursor();
    } else {
        QWindowSystemInterface::handleLeaveEvent(window());
    }
}

void QWaylandWindow::handleMouseEventWithDecoration(QWaylandInputDevice *inputDevice, ulong timestamp, const QPointF &local, const QPointF &global, Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    if (mWindowDecoration->handleMouse(inputDevice,local,global,b,mods))
        return;

    QMargins marg = frameMargins();
    QRect windowRect(0 + marg.left(),
                     0 + marg.top(),
                     geometry().size().width() - marg.right(),
                     geometry().size().height() - marg.bottom());
    if (windowRect.contains(local.toPoint()) || mMousePressedInContentArea != Qt::NoButton) {
        QPointF localTranslated = local;
        QPointF globalTranslated = global;
        localTranslated.setX(localTranslated.x() - marg.left());
        localTranslated.setY(localTranslated.y() - marg.top());
        globalTranslated.setX(globalTranslated.x() - marg.left());
        globalTranslated.setY(globalTranslated.y() - marg.top());
        if (!mMouseEventsInContentArea) {
            mWindowDecoration->restoreMouseCursor();
            QWindowSystemInterface::handleEnterEvent(window());
        }
        QWindowSystemInterface::handleMouseEvent(window(), timestamp, localTranslated, globalTranslated, b, mods);
        mMouseEventsInContentArea = true;
        mMousePressedInContentArea = b;
    } else {
        if (mMouseEventsInContentArea) {
            QWindowSystemInterface::handleLeaveEvent(window());
            mMouseEventsInContentArea = false;
        }
        mWindowDecoration->handleMouse(inputDevice,local,global,b,mods);
    }
}
