//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: mtscale.h,v 1.3 2004/04/27 22:27:06 spamatica Exp $
//  (C) Copyright 1999 Werner Schweer (ws@seh.de)
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================

#ifndef __MTSCALE_H__
#define __MTSCALE_H__

#include "type_defs.h"
#include "view.h"


namespace MusEGui {

//---------------------------------------------------------
//   MTScale
//    scale for midi track
//---------------------------------------------------------

class MTScale : public View {
      Q_OBJECT
      int raster;
      unsigned pos[4];
      int button;
      bool barLocator;
      bool waveMode;

   private slots:
      void songChanged(MusECore::SongChangedStruct_t);
      void configChanged();

   protected:
      virtual void pdraw(QPainter&, const QRect&, const QRegion& = QRegion());
      virtual void viewMousePressEvent(QMouseEvent* event);
      virtual void viewMouseMoveEvent(QMouseEvent* event);
      virtual void viewMouseReleaseEvent(QMouseEvent* event);
      virtual void leaveEvent(QEvent*e);

      virtual void drawTickRaster(QPainter& p, const QRect&, const QRegion& = QRegion(), int raster = 0,
                                      bool waveMode = false,
                                      bool useGivenColors = false,
                                      bool drawText = false,
                                      const QColor& bar_color = Qt::cyan, // default to very obvious color to visualize uninitialized drawing
                                      const QColor& beat_color = Qt::cyan,
                                      const QColor& fine_color = Qt::cyan,
                                      const QColor& coarse_color = Qt::cyan,
                                      const QColor& text_color = Qt::cyan,
                                      const QFont& large_font = QFont(),
                                      const QFont& small_font = QFont()
                                     );

   signals:
      void timeChanged(unsigned);

   public slots:
      void setPos(int, unsigned, bool);
      void setRaster(int);

   public:
      MTScale(int raster, QWidget* parent, int xscale, bool f = false);
      void setBarLocator(bool f) { barLocator = f; }
      };

} 

#endif

