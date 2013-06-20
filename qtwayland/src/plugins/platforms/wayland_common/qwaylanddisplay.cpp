/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the plugins of the Qt Toolkit.
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

#include "qwaylanddisplay.h"

#include "qwaylandeventthread.h"
#include "qwaylandwindow.h"
#include "qwaylandscreen.h"
#include "qwaylandcursor.h"
#include "qwaylandinputdevice.h"
#include "qwaylandclipboard.h"
#include "qwaylanddatadevicemanager.h"

#ifdef QT_WAYLAND_GL_SUPPORT
#include "qwaylandglintegration.h"
#endif

#include "qwaylandwindowmanagerintegration.h"

#include "qwaylandextendedoutput.h"
#include "qwaylandextendedsurface.h"
#include "qwaylandsubsurface.h"
#include "qwaylandtouch.h"
#include "qwaylandqtkey.h"

#include <QtCore/QAbstractEventDispatcher>
#include <QtGui/private/qguiapplication_p.h>

#include <QtCore/QDebug>

QT_USE_NAMESPACE

struct wl_surface *QWaylandDisplay::createSurface(void *handle)
{
    struct wl_surface *surface = mCompositor.create_surface();
    wl_surface_set_user_data(surface, handle);
    return surface;
}

#ifdef QT_WAYLAND_GL_SUPPORT
QWaylandGLIntegration * QWaylandDisplay::eglIntegration()
{
    return mEglIntegration;
}
#endif

QWaylandWindowManagerIntegration *QWaylandDisplay::windowManagerIntegration()
{
    return mWindowManagerIntegration;
}

QWaylandInputDevice *QWaylandDisplay::lastKeyboardFocusInputDevice() const
{
    return mLastKeyboardFocusInputDevice;
}

void QWaylandDisplay::setLastKeyboardFocusInputDevice(QWaylandInputDevice *device)
{
    mLastKeyboardFocusInputDevice = device;
}

static QWaylandDisplay *display = 0;

QWaylandDisplay::QWaylandDisplay()
    : mLastKeyboardFocusInputDevice(0)
    , mDndSelectionHandler(0)
    , mWindowExtension(0)
    , mSubSurfaceExtension(0)
    , mOutputExtension(0)
    , mTouchExtension(0)
    , mQtKeyExtension(0)
{
    display = this;
    qRegisterMetaType<uint32_t>("uint32_t");

    mEventThreadObject = new QWaylandEventThread(0);
    mEventThread = new QThread(this);
    mEventThreadObject->moveToThread(mEventThread);
    mEventThread->start();

    mEventThreadObject->displayConnect();
    mDisplay = mEventThreadObject->display(); //blocks until display is available

    //Create a new even queue for the QtGui thread
    mEventQueue = wl_display_create_queue(mDisplay);

    struct ::wl_registry *registry = wl_display_get_registry(mDisplay);
    wl_proxy_set_queue((struct wl_proxy *)registry, mEventQueue);

    init(registry);

    QAbstractEventDispatcher *dispatcher = QGuiApplicationPrivate::eventDispatcher;
    connect(dispatcher, SIGNAL(aboutToBlock()), this, SLOT(flushRequests()));
    connect(mEventThreadObject, SIGNAL(newEventsRead()), this, SLOT(flushRequests()));

#ifdef QT_WAYLAND_GL_SUPPORT
    mEglIntegration = QWaylandGLIntegration::createGLIntegration(this);
#endif

    mWindowManagerIntegration = new QWaylandWindowManagerIntegration(this);

    blockingReadEvents();

#ifdef QT_WAYLAND_GL_SUPPORT
    mEglIntegration->initialize();

    flushRequests();
    while (mEglIntegration->waitingForEvents())
        blockingReadEvents();
#endif

    waitForScreens();
}

QWaylandDisplay::~QWaylandDisplay(void)
{
#ifdef QT_WAYLAND_GL_SUPPORT
    delete mEglIntegration;
#endif
    mEventThread->quit();
    mEventThread->wait();
    delete mEventThreadObject;
}

void QWaylandDisplay::flushRequests()
{
    wl_display_dispatch_queue_pending(mDisplay, mEventQueue);
    wl_display_flush(mDisplay);
}

void QWaylandDisplay::blockingReadEvents()
{
    wl_display_dispatch_queue(mDisplay, mEventQueue);
}

QWaylandScreen *QWaylandDisplay::screenForOutput(struct wl_output *output) const
{
    for (int i = 0; i < mScreens.size(); ++i) {
        QWaylandScreen *screen = static_cast<QWaylandScreen *>(mScreens.at(i));
        if (screen->output() == output)
            return screen;
    }
    return 0;
}

void QWaylandDisplay::addRegistryListener(RegistryListener listener, void *data)
{
    Listener l = { listener, data };
    mRegistryListeners.append(l);
}

void QWaylandDisplay::waitForScreens()
{
    flushRequests();

    while (true) {
        bool screensReady = !mScreens.isEmpty();

        for (int ii = 0; screensReady && ii < mScreens.count(); ++ii) {
            if (mScreens.at(ii)->geometry() == QRect(0, 0, 0, 0))
                screensReady = false;
        }

        if (!screensReady)
            blockingReadEvents();
        else
            return;
    }
}

void QWaylandDisplay::registry_global(uint32_t id, const QString &interface, uint32_t version)
{
    Q_UNUSED(version);

    struct ::wl_registry *registry = object();

    if (interface == "wl_output") {
        mScreens.append(new QWaylandScreen(this, id));
    } else if (interface == "wl_compositor") {
        mCompositor.init(registry, id);
    } else if (interface == "wl_shm") {
        mShm = static_cast<struct wl_shm *>(wl_registry_bind(registry, id, &wl_shm_interface,1));
    } else if (interface == "wl_shell"){
        mShell = new QtWayland::wl_shell(registry, id);
    } else if (interface == "wl_seat") {
        QWaylandInputDevice *inputDevice = new QWaylandInputDevice(this, id);
        mInputDevices.append(inputDevice);
    } else if (interface == "wl_data_device_manager") {
        mDndSelectionHandler = new QWaylandDataDeviceManager(this, id);
    } else if (interface == "qt_output_extension") {
        mOutputExtension = new QtWayland::qt_output_extension(registry, id);
        foreach (QPlatformScreen *screen, screens())
            static_cast<QWaylandScreen *>(screen)->createExtendedOutput();
    } else if (interface == "qt_surface_extension") {
        mWindowExtension = new QtWayland::qt_surface_extension(registry, id);
    } else if (interface == "qt_sub_surface_extension") {
        mSubSurfaceExtension = new QtWayland::qt_sub_surface_extension(registry, id);
    } else if (interface == "qt_touch_extension") {
        mTouchExtension = new QWaylandTouchExtension(this, id);
    } else if (interface == "qt_qtkey_extension") {
        mQtKeyExtension = new QWaylandQtKeyExtension(this, id);
    }

    foreach (Listener l, mRegistryListeners)
        (*l.listener)(l.data, registry, id, interface, version);
}

uint32_t QWaylandDisplay::currentTimeMillisec()
{
    //### we throw away the time information
    struct timeval tv;
    int ret = gettimeofday(&tv, 0);
    if (ret == 0)
        return tv.tv_sec*1000 + tv.tv_usec/1000;
    return 0;
}

void QWaylandDisplay::forceRoundTrip()
{
    wl_display_roundtrip(mDisplay);
}

