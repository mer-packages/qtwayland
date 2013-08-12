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

#ifndef QWAYLANDDATAOFFER_H
#define QWAYLANDDATAOFFER_H

#include <QString>
#include <QByteArray>
#include <QMimeData>

#include <QtGui/private/qdnd_p.h>
#include <QtGui/QClipboard>

#include <stdint.h>

struct wl_callback;
struct wl_callback_listener;
struct wl_data_offer;
struct wl_data_offer_listener;

QT_BEGIN_NAMESPACE

class QWaylandDisplay;

class QWaylandDataOffer : public QInternalMimeData
{
public:
    QWaylandDataOffer(QWaylandDisplay *display, struct wl_data_offer *offer);
    ~QWaylandDataOffer();

    bool hasFormat_sys(const QString &mimeType) const;
    QStringList formats_sys() const;
    QVariant retrieveData_sys(const QString &mimeType, QVariant::Type type) const;

    struct wl_data_offer *handle() const;
private:

    struct wl_data_offer *m_data_offer;
    QWaylandDisplay *m_display;
    QStringList m_offered_mime_types;
    wl_callback *m_receiveSyncCallback;

    static void offer(void *data, struct wl_data_offer *wl_data_offer, const char *type);
    static const struct wl_data_offer_listener data_offer_listener;

    static void offer_sync_callback(void *data, struct wl_callback *wl_callback, uint32_t time);
    static const struct wl_callback_listener offer_sync_callback_listener;
};

QT_END_NAMESPACE

#endif
