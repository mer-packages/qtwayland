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
    , m_textureIdBufferNeedsDisown(false)
    , m_textureIdBuffer(0)
{
    wl_list_init(&m_frame_callback_list);
    wl_list_init(&m_pending_frame_callback_list);

    for (int i = 0; i < buffer_pool_size; i++)
        m_bufferPool[i] = new SurfaceBuffer(this);
}

Surface::~Surface()
{
    delete m_waylandSurface;
    delete m_subSurface;

    for (int i = 0; i < buffer_pool_size; i++) {
        if (!m_bufferPool[i]->pageFlipperHasBuffer())
            delete m_bufferPool[i];
    }
}

Surface *Surface::fromResource(struct ::wl_resource *resource)
{
    return static_cast<Surface *>(Resource::fromResource(resource)->surface);
}

QWaylandSurface::Type Surface::type() const
{
    SurfaceBuffer *surfaceBuffer = currentSurfaceBuffer();
    if (surfaceBuffer && surfaceBuffer->waylandBufferHandle()) {
        if (surfaceBuffer->isShmBuffer()) {
            return QWaylandSurface::Shm;
        } else {
            return QWaylandSurface::Texture;
        }
    }
    return QWaylandSurface::Invalid;
}

bool Surface::isYInverted() const
{
    bool ret = false;
    static bool negateReturn = qgetenv("QT_COMPOSITOR_NEGATE_INVERTED_Y").toInt();
    QWaylandGraphicsHardwareIntegration *graphicsHWIntegration = m_compositor->graphicsHWIntegration();

#ifdef QT_COMPOSITOR_WAYLAND_GL
    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    if (!surfacebuffer) {
        ret = false;
    } else if (graphicsHWIntegration && surfacebuffer->waylandBufferHandle() && type() != QWaylandSurface::Shm) {
        ret = graphicsHWIntegration->isYInverted(surfacebuffer->waylandBufferHandle());
    } else
#endif
        ret = true;

    return ret != negateReturn;
}

bool Surface::visible() const
{

    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    return surfacebuffer->waylandBufferHandle();
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
    SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();
    if (surfacebuffer && !surfacebuffer->isDestroyed() && type() == QWaylandSurface::Shm) {
        return surfacebuffer->image();
    }
    return QImage();
}

#ifdef QT_COMPOSITOR_WAYLAND_GL
GLuint Surface::textureId(QOpenGLContext *context) const
{
    const SurfaceBuffer *surfacebuffer = currentSurfaceBuffer();

    if (m_compositor->graphicsHWIntegration() && type() == QWaylandSurface::Texture
         && !surfacebuffer->textureCreated()) {
        QWaylandGraphicsHardwareIntegration *hwIntegration = m_compositor->graphicsHWIntegration();
        const_cast<SurfaceBuffer *>(surfacebuffer)->createTexture(hwIntegration,context);
    }

    if (m_textureIdBufferNeedsDisown) {
        Q_ASSERT(m_textureIdBuffer);
        const_cast<SurfaceBuffer *>(m_textureIdBuffer)->disown();
        m_textureIdBufferNeedsDisown = false;
    }
    m_textureIdBuffer = surfacebuffer;

    return surfacebuffer->texture();
}
#endif // QT_COMPOSITOR_WAYLAND_GL

void Surface::sendFrameCallback()
{
    SurfaceBuffer *surfaceBuffer = currentSurfaceBuffer();
    if (surfaceBuffer)
        surfaceBuffer->setDisplayed();

    if (m_backBuffer) {
        if (m_frontBuffer) {
#ifdef QT_COMPOSITOR_WAYLAND_GL
            if (m_textureIdBuffer == m_frontBuffer) {
                m_textureIdBufferNeedsDisown = true;
            } else {
#endif
                m_frontBuffer->disown();
#ifdef QT_COMPOSITOR_WAYLAND_GL
            }
#endif
        }
        m_frontBuffer = m_backBuffer;
    }

    bool updateNeeded = advanceBufferQueue();

    uint time = Compositor::currentTimeMsecs();
    struct wl_resource *frame_callback, *next;
    wl_list_for_each_safe(frame_callback, next, &m_frame_callback_list, link) {
        wl_callback_send_done(frame_callback, time);
        wl_resource_destroy(frame_callback);
    }
    wl_list_init(&m_frame_callback_list);

    if (updateNeeded)
        doUpdate();
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

bool Surface::advanceBufferQueue()
{
    //has current buffer been displayed,
    //do we have another buffer in the queue
    //and does it have a valid damage rect

    if (m_backBuffer && !m_backBuffer->isDisplayed())
        return true;
    if (m_bufferQueue.size()) {
        QSize size;
        if (m_backBuffer && m_backBuffer->waylandBufferHandle()) {
            size = m_backBuffer->size();
        }

        if (!m_bufferQueue.first()->isComitted())
            return false;

        m_backBuffer = m_bufferQueue.takeFirst();
        while (m_backBuffer && m_backBuffer->isDestroyed()) {
            m_backBuffer->disown();
            m_backBuffer = m_bufferQueue.size() ? m_bufferQueue.takeFirst() : 0;
        }

        if (!m_backBuffer)
            return false; //we have no new backbuffer;

        if (m_backBuffer->waylandBufferHandle()) {
            size = m_backBuffer->size();
        }
        setSize(size);


        if (m_backBuffer &&  (!m_subSurface || !m_subSurface->parent()) && !m_surfaceMapped) {
            m_surfaceMapped = true;
            emit m_waylandSurface->mapped();
        } else if (m_backBuffer && !m_backBuffer->waylandBufferHandle() && m_surfaceMapped) {
            m_surfaceMapped = false;
            emit m_waylandSurface->unmapped();
        }

    } else {
        m_backBuffer = 0;
        return false;
    }

    return true;
}

void Surface::doUpdate() {
    if (postBuffer()) {
#ifdef QT_COMPOSITOR_QUICK
        QWaylandSurfaceItem *surfaceItem = waylandSurface()->surfaceItem();
        if (surfaceItem)
            surfaceItem->setDamagedFlag(true); // avoid flicker when we switch back to composited mode
#endif
        sendFrameCallback();
    } else {
        SurfaceBuffer *surfaceBuffer = currentSurfaceBuffer();
        if (surfaceBuffer && surfaceBuffer->damageRect().isValid()) {
            emit m_waylandSurface->damaged(surfaceBuffer->damageRect());
        } else {
            sendFrameCallback();
        }
    }
}

SurfaceBuffer *Surface::createSurfaceBuffer(struct ::wl_resource *buffer)
{
    SurfaceBuffer *newBuffer = 0;
    for (int i = 0; i < Surface::buffer_pool_size; i++) {
        if (!m_bufferPool[i]->isRegisteredWithBuffer()) {
            newBuffer = m_bufferPool[i];
            newBuffer->initialize(buffer);
            break;
        }
    }

    Q_ASSERT(newBuffer);
    return newBuffer;
}

bool Surface::postBuffer() {
#ifdef QT_COMPOSITOR_WAYLAND_GL
    if (m_waylandSurface->handle() == m_compositor->directRenderSurface()) {
        SurfaceBuffer *surfaceBuffer = currentSurfaceBuffer();
        if (surfaceBuffer && surfaceBuffer->waylandBufferHandle()) {
            if (m_compositor->pageFlipper()) {
                if (m_compositor->pageFlipper()->displayBuffer(surfaceBuffer)) {
                    surfaceBuffer->setPageFlipperHasBuffer(true);
                    m_compositor->setDirectRenderingActive(true);
                    return true;
                } else {
                    if (QT_WAYLAND_PRINT_BUFFERING_WARNINGS)
                        qWarning() << "could not post buffer";
                }
            }
        }
    }
#endif
    return false;
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

    if (!buffer) {
        InputDevice *inputDevice = m_compositor->defaultInputDevice();
        if (inputDevice->keyboardFocus() == this)
            inputDevice->setKeyboardFocus(0);
        if (inputDevice->mouseFocus() == this)
            inputDevice->setMouseFocus(0, QPointF(), QPointF());
    }
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
    compositor()->surfaceDestroyed(this);
    delete this;
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
        advanceBufferQueue();
    }

    doUpdate();
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
