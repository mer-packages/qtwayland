/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Compositor.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of Digia Plc and its Subsidiary(-ies) nor the names
**     of its contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwlsurface_p.h"

#include "qwaylandsurface.h"
#ifdef QT_COMPOSITOR_QUICK
#include "qwaylandsurfaceitem.h"
#endif

#include "qwlcompositor_p.h"
#include "qwlinputdevice_p.h"
#include "qwlextendedsurface_p.h"
#include "qwlregion_p.h"
#include "qwlsubsurface_p.h"
#include "qwlsurfacebuffer_p.h"
#include "qwlshellsurface_p.h"

#include <QtCore/QDebug>
#include <QTouchEvent>
#include <QtCore/QCoreApplication>

#include <wayland-server.h>

#ifdef QT_COMPOSITOR_WAYLAND_GL
#include "hardware_integration/qwaylandgraphicshardwareintegration.h"
#include <qpa/qplatformopenglcontext.h>
#endif

#ifdef QT_WAYLAND_WINDOWMANAGER_SUPPORT
#include "waylandwindowmanagerintegration.h"
#endif

QT_BEGIN_NAMESPACE

namespace QtWayland {

static bool QT_WAYLAND_PRINT_BUFFERING_WARNINGS = qEnvironmentVariableIsSet("QT_WAYLAND_PRINT_BUFFERING_WARNINGS");

Surface::Surface(struct wl_client *client, uint32_t id, Compositor *compositor)
    : QtWaylandServer::wl_surface(client, id)
    , m_compositor(compositor)
    , m_waylandSurface(new QWaylandSurface(this))
    , m_backBuffer(0)
    , m_frontBuffer(0)
    , m_surfaceMapped(false)
    , m_extendedSurface(0)
    , m_subSurface(0)
    , m_shellSurface(0)
    , m_transientInactive(false)
    , m_isCursorSurface(false)
    , m_visible(false)
    , m_invertY(false)
    , m_bufferType(QWaylandSurface::Invalid)
    , m_surfaceWasDestroyed(false)
    , m_deleteGuard(false)
{
    wl_list_init(&m_frame_callback_list);
    wl_list_init(&m_pending_frame_callback_list);
}

Surface::~Surface()
{
    delete m_waylandSurface;
    delete m_subSurface;

    for (int i = 0; i < m_bufferPool.size(); i++) {
        if (!m_bufferPool[i]->pageFlipperHasBuffer())
            delete m_bufferPool[i];
    }
}

void Surface::releaseSurfaces()
{
    delete m_waylandSurface;
    m_waylandSurface = 0;
    delete m_subSurface;
    m_subSurface = 0;
}

void Surface::releaseFrontBuffer()
{
    if (m_frontBuffer && m_frontBuffer->waylandBufferHandle() && m_frontBuffer->isShmBuffer()) {
        m_frontBuffer->disown();
        m_frontBuffer = 0;
    }
}

Surface *Surface::fromResource(struct ::wl_resource *resource)
{
    return static_cast<Surface *>(Resource::fromResource(resource)->surface);
}

QWaylandSurface::Type Surface::type() const
{
    return m_bufferType;
}

bool Surface::isYInverted() const
{
    static bool negateReturn = qgetenv("QT_COMPOSITOR_NEGATE_INVERTED_Y").toInt();
    return m_invertY != negateReturn;
}

/*
 * When the compositor goes of screen, we want to release as much as
 * possible, but we retain one buffer so we have something to
 * represent the client when compositor comes back. This buffer is set
 * as the backbuffer (similar to what we do in surface_commit below).
 *
 * For hardware buffers, we retain the front buffer because we are not
 * on the render thread and we don't have GL to release. This applies
 * to QtQuick only, but is generalized for simplicity. This means that
 * a client which renders with only two hardware buffers will
 * potentially have both those buffers in the compositor when it goes
 * off screen and the client can thus be blocked. Qt clients use 3
 * buffers though, so this is not a problem in practice.
 *
 * For SHM buffers, the memory is already bound as a texture so we can
 * release it regardless. That means that SHM clients are ok with only
 * 2 buffers.
 */
void Surface::setCompositorVisible(bool visible)
{
    if (!visible) {
        if (m_bufferQueue.size() > 0) {
            // If there are buffers in the queue, cycle through them
            // and set the last one as backbuffer. Release all others.
            if (m_backBuffer)
                m_backBuffer->disown();
            while (m_bufferQueue.size() > 1)
                m_bufferQueue.takeFirst()->disown();
            setBackBuffer(m_bufferQueue.takeLast());
            Q_ASSERT(m_bufferQueue.isEmpty());
        }
        if (m_frontBuffer && type() == QWaylandSurface::Shm) {
            // Release front shm buffer.
            m_frontBuffer->disown();
            m_frontBuffer = 0;
        }
    }
}

bool Surface::visible() const
{
    return m_visible;
}

QPointF Surface::pos() const
{
    return m_shellSurface ? m_shellSurface->adjustedPosToTransientParent() : m_position;
}

QPointF Surface::nonAdjustedPos() const
{
    return m_position;
}

void Surface::setPos(const QPointF &pos)
{
    bool emitChange = pos != m_position;
    m_position = pos;
    if (emitChange)
        m_waylandSurface->posChanged();
}

QSize Surface::size() const
{
    return m_size;
}

void Surface::setSize(const QSize &size)
{
    if (size != m_size) {
        m_opaqueRegion = QRegion();
        m_inputRegion = QRegion(QRect(QPoint(), size));
        m_size = size;
        if (m_shellSurface) {
            m_shellSurface->adjustPosInResize();
        }
        m_waylandSurface->sizeChanged();
    }
}

QRegion Surface::inputRegion() const
{
    return m_inputRegion;
}

QRegion Surface::opaqueRegion() const
{
    return m_opaqueRegion;
}

QImage Surface::image() const
{
    if (m_frontBuffer && !m_frontBuffer->isDestroyed() && m_bufferType == QWaylandSurface::Shm)
        return m_frontBuffer->image();
    return QImage();
}

#ifdef QT_COMPOSITOR_WAYLAND_GL
GLuint Surface::textureId(QOpenGLContext *context) const
{
    const SurfaceBuffer *surfacebuffer = m_frontBuffer;

    if (m_compositor->graphicsHWIntegration() && type() == QWaylandSurface::Texture
         && !surfacebuffer->textureCreated()) {
        QWaylandGraphicsHardwareIntegration *hwIntegration = m_compositor->graphicsHWIntegration();
        const_cast<SurfaceBuffer *>(surfacebuffer)->createTexture(hwIntegration,context);
    }
    return surfacebuffer->texture();
}
#endif // QT_COMPOSITOR_WAYLAND_GL

void Surface::sendFrameCallback()
{
    uint time = Compositor::currentTimeMsecs();
    struct wl_resource *frame_callback, *next;
    wl_list_for_each_safe(frame_callback, next, &m_frame_callback_list, link) {
        wl_callback_send_done(frame_callback, time);
        wl_resource_destroy(frame_callback);
    }
    wl_list_init(&m_frame_callback_list);
}

void Surface::frameFinished()
{
    m_compositor->frameFinished(this);
}

QWaylandSurface * Surface::waylandSurface() const
{
    return m_waylandSurface;
}

QPoint Surface::lastMousePos() const
{
    return m_lastLocalMousePos;
}

void Surface::setExtendedSurface(ExtendedSurface *extendedSurface)
{
    m_extendedSurface = extendedSurface;
    if (m_extendedSurface)
        emit m_waylandSurface->extendedSurfaceReady();
}

ExtendedSurface *Surface::extendedSurface() const
{
    return m_extendedSurface;
}

void Surface::setSubSurface(SubSurface *subSurface)
{
    m_subSurface = subSurface;
}

SubSurface *Surface::subSurface() const
{
    return m_subSurface;
}

void Surface::setShellSurface(ShellSurface *shellSurface)
{
    m_shellSurface = shellSurface;
}

ShellSurface *Surface::shellSurface() const
{
    return m_shellSurface;
}

Compositor *Surface::compositor() const
{
    return m_compositor;
}

/*
 * This little curveball is here to prevent the surface from
 * being deleted while there are queued connections pending from
 * QWaylandSurface which have yet to be delivered to receivers on
 * the GUI thread.
 *
 * There is a slight chance that a surface_destroy_resource
 * comes on the GUI thread and enters the GUI thread's event queue
 * during the sync phase, before queued connections from
 * 'sizeChanged', 'mapped' and 'unmapped' which are fired
 * during advanceBufferQueue.
 *
 * The delete guard is here to delay the destruction until after
 * these queued connections have been properly delivered. This
 * is done by posting an event at the very end of
 * advanceBufferQueue(). Once this has been delivered on the GUI
 * thread, it is safe to delete again. Though ugly, the delay and
 * overhead is minimal.
 */
void Surface::enterDeleteGuard()
{
    m_deleteGuard = true;
    QCoreApplication::postEvent(m_compositor, new DeleteGuard(this));
}

void Surface::leaveDeleteGuard()
{
    if (m_surfaceWasDestroyed)
        m_compositor->destroySurface(this);
    m_deleteGuard = false;
}

void Surface::advanceBufferQueue()
 {
    SurfaceBuffer *front = m_frontBuffer;

    // Advance current back buffer to the front buffer.
    if (m_backBuffer) {
        if (m_backBuffer->isDestroyed()) {
            m_backBuffer->disown();
            m_backBuffer = 0;
         }
        m_frontBuffer = m_backBuffer;
        m_backBuffer = 0;
    }

    // Set a new back buffer if there is something in the queue.
    if (m_bufferQueue.size() && m_bufferQueue.first()->isComitted()) {
        SurfaceBuffer *next = m_bufferQueue.takeFirst();
        while (next && next->isDestroyed()) {
            next->disown();
            next = m_bufferQueue.size() ? m_bufferQueue.takeFirst() : 0;
        }
        setBackBuffer(next);
    }

    // Release the old front buffer if we changed it.
    if (front && front != m_frontBuffer)
        front->disown();

    enterDeleteGuard();
}

/*!
 * Sets the backbuffer for this surface. The back buffer is not yet on
 * screen and will become live during the next advanceBufferQueue().
 *
 * The backbuffer represents the current state of the surface for the
 * purpose of GUI-thread accessible properties such as size and visibility.
 */
void Surface::setBackBuffer(SurfaceBuffer *buffer)
{
    m_backBuffer = buffer;

    if (m_backBuffer) {
        m_visible = m_backBuffer->waylandBufferHandle() != 0;
        QWaylandGraphicsHardwareIntegration *hwi = m_compositor->graphicsHWIntegration();
        m_invertY = m_visible && (buffer->isShmBuffer() || (hwi && hwi->isYInverted(buffer->waylandBufferHandle())));
        m_bufferType = m_visible ? (buffer->isShmBuffer() ? QWaylandSurface::Shm : QWaylandSurface::Texture) : QWaylandSurface::Invalid;

        setSize(m_visible ? m_backBuffer->size() : QSize());

        if ((!m_subSurface || !m_subSurface->parent()) && !m_surfaceMapped) {
             m_surfaceMapped = true;
             emit m_waylandSurface->mapped();
        } else if (!m_visible && m_surfaceMapped) {
             m_surfaceMapped = false;
             emit m_waylandSurface->unmapped();
        }

        m_compositor->markSurfaceAsDirty(this);
        emit m_waylandSurface->damaged(m_backBuffer->damageRect());
    } else {
        m_visible = false;
        m_invertY = false;
        m_bufferType = QWaylandSurface::Invalid;

        InputDevice *inputDevice = m_compositor->defaultInputDevice();
        if (inputDevice->keyboardFocus() == this)
            inputDevice->setKeyboardFocus(0);
        if (inputDevice->mouseFocus() == this)
            inputDevice->setMouseFocus(0, QPointF(), QPointF());
    }
}

SurfaceBuffer *Surface::createSurfaceBuffer(struct ::wl_resource *buffer)
{
    SurfaceBuffer *newBuffer = 0;
    for (int i = 0; i < m_bufferPool.size(); i++) {
        if (!m_bufferPool[i]->isRegisteredWithBuffer()) {
            newBuffer = m_bufferPool[i];
            newBuffer->initialize(buffer);
            break;
        }
    }

    if (!newBuffer) {
        newBuffer = new SurfaceBuffer(this);
        newBuffer->initialize(buffer);
        m_bufferPool.append(newBuffer);
        if (m_bufferPool.size() > 3)
            qWarning() << Q_FUNC_INFO << "Increased buffer pool size to" << m_bufferPool.size() << "for surface with title:" << title() << "className:" << className();
    }

    return newBuffer;
}

void Surface::attach(struct ::wl_resource *buffer)
{
    SurfaceBuffer *last = m_bufferQueue.size()?m_bufferQueue.last():0;
    if (last) {
        if (last->waylandBufferHandle() == buffer) {
            if (QT_WAYLAND_PRINT_BUFFERING_WARNINGS)
                qWarning() << "attaching already attached buffer";
            return;
        }
        if (!last->damageRect().isValid() || !last->isComitted() || isCursorSurface() ){
            last->disown();
            m_bufferQueue.takeLast();
        }
    }

    SurfaceBuffer *surfBuf = createSurfaceBuffer(buffer);
    m_bufferQueue << surfBuf;
}

void Surface::damage(const QRect &rect)
{
    if (m_bufferQueue.empty()) {
        if (QT_WAYLAND_PRINT_BUFFERING_WARNINGS)
            qWarning() << "Surface::damage() null buffer";
        return;
    }
    SurfaceBuffer *surfaceBuffer =  m_bufferQueue.last();
    if (surfaceBuffer) {
        if (surfaceBuffer->isComitted()) {
            if (QT_WAYLAND_PRINT_BUFFERING_WARNINGS)
                qWarning("Surface::damage() on a committed surface");
        } else{
            surfaceBuffer->setDamage(rect);
        }
    }
}

void Surface::surface_destroy_resource(Resource *)
{
    if (m_deleteGuard)
        m_surfaceWasDestroyed = true;
    else
        compositor()->destroySurface(this);
}

void Surface::surface_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void Surface::surface_attach(Resource *, struct wl_resource *buffer, int x, int y)
{
    Q_UNUSED(x);
    Q_UNUSED(y);

    attach(buffer);
}

void Surface::surface_damage(Resource *, int32_t x, int32_t y, int32_t width, int32_t height)
{
    damage(QRect(x, y, width, height));
}

void Surface::surface_frame(Resource *resource, uint32_t callback)
{
    struct wl_resource *frame_callback = wl_client_add_object(resource->client(), &wl_callback_interface, 0, callback, this);
    wl_list_insert(&m_pending_frame_callback_list, &frame_callback->link);
}

void Surface::surface_set_opaque_region(Resource *, struct wl_resource *region)
{
    m_opaqueRegion = region ? Region::fromResource(region)->region() : QRegion();
}

void Surface::surface_set_input_region(Resource *, struct wl_resource *region)
{
    m_inputRegion = region ? Region::fromResource(region)->region() : QRegion(QRect(QPoint(), size()));
}

void Surface::surface_commit(Resource *)
{
    if (m_bufferQueue.empty()) {
        if (QT_WAYLAND_PRINT_BUFFERING_WARNINGS)
            qWarning("Commit on invalid surface");
        return;
    }

    if (!wl_list_empty(&m_pending_frame_callback_list))
        m_compositor->markSurfaceAsDirty(this);

    if (!wl_list_empty(&m_frame_callback_list))
            qWarning("Previous callbacks not sent yet, client is not waiting for callbacks?");

    wl_list_insert_list(&m_frame_callback_list, &m_pending_frame_callback_list);
    wl_list_init(&m_pending_frame_callback_list);

    SurfaceBuffer *surfaceBuffer = m_bufferQueue.last();
    if (surfaceBuffer) {
        if (surfaceBuffer->isComitted()) {
            if (QT_WAYLAND_PRINT_BUFFERING_WARNINGS)
                qWarning("Committing buffer that has already been committed");
        } else {
            surfaceBuffer->setCommitted();
        }
    }

    if (compositor() && compositor()->window() && !compositor()->window()->isVisible()) {
        if (m_backBuffer)
            m_backBuffer->disown();
        while (m_bufferQueue.size() > 1) // keep the last buffer to have something to show.
            m_bufferQueue.takeFirst()->disown();
        setBackBuffer(surfaceBuffer);
        m_bufferQueue.clear();
        return;
    }

    // A new buffer was added to the queue, so we set it as the current
    // back buffer. Second and third buffers, if the come, will be handled
    // in advanceBufferQueue().
    if (!m_backBuffer && m_bufferQueue.size() == 1) {
        setBackBuffer(surfaceBuffer);
        m_bufferQueue.takeFirst();
    }
}

void Surface::setClassName(const QString &className)
{
    if (m_className != className) {
        m_className = className;
        emit waylandSurface()->classNameChanged();
    }
}

void Surface::setTitle(const QString &title)
{
    if (m_title != title) {
        m_title = title;
        emit waylandSurface()->titleChanged();
    }
}

} // namespace Wayland

QT_END_NAMESPACE
