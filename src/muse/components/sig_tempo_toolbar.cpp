//=========================================================
//  MusE
//  Linux Music Editor
//  sig_tempo_toolbar.cpp
//  (C) Copyright 2012 Florian Jung (flo93@users.sourceforge.net)
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


#include "sig_tempo_toolbar.h"
#include "tempolabel.h"
#include "sigedit.h"
#include "song.h"
#include "icons.h"
#include "sig.h"
#include "tempo.h"

#include <QLabel>
#include <QToolButton>

namespace MusEGui
{

//---------------------------------
//   TempoToolbar
//---------------------------------

TempoToolbar::TempoToolbar(QWidget* parent)
    : QToolBar(parent)
{
    init();
}

TempoToolbar::TempoToolbar(const QString& title, QWidget* parent)
    : QToolBar(title, parent)
{
    init();
}

void TempoToolbar::init()
{
    setObjectName("Tempo toolbar");

    _masterButton = new QToolButton(this);
    _masterButton->setIcon(*masterTrackOnSVGIcon);
    _masterButton->setCheckable(true);
    _masterButton->setToolTip(tr("Use mastertrack tempo"));
    connect(_masterButton, SIGNAL(toggled(bool)), SLOT(masterToggled(bool)));

    tempo_edit=new TempoEdit(this);
    tempo_edit->setToolTip(tr("Mastertrack tempo at current position, or fixed tempo"));
    tempo_edit->setContentsMargins(0, 0, 0, 0);
    tempo_edit->setFocusPolicy(Qt::StrongFocus);

    tap_button = new QToolButton(this);
    tap_button->setText(tr("Tap"));
    tap_button->setToolTip(tr("Tap this button to set tempo"));
    tap_button->setStatusTip(tr("Set the tempo to the time difference of two successive clicks. Times out after two seconds."));
    tap_button->setContentsMargins(0, 0, 0, 0);

    blink_timer = new QTimer(this);
    connect(blink_timer, SIGNAL(timeout()), this, SLOT(tapButtonBlink()));
    blink_timer->stop();

    //   addWidget(_externSyncButton);
    addWidget(_masterButton);
    addWidget(tempo_edit);
    addWidget(tap_button);

    connect(MusEGlobal::song, SIGNAL(songChanged(MusECore::SongChangedStruct_t)), this, SLOT(song_changed(MusECore::SongChangedStruct_t)));
    connect(MusEGlobal::song, SIGNAL(posChanged(int, unsigned, bool)), this, SLOT(pos_changed(int,unsigned,bool)));
    //connect(&MusEGlobal::extSyncFlag, SIGNAL(valueChanged(bool)), SLOT(syncChanged(bool)));

    connect(tempo_edit, SIGNAL(tempoChanged(double)), MusEGlobal::song, SLOT(setTempo(double)));
    connect(tempo_edit, SIGNAL(returnPressed()), SIGNAL(returnPressed()));
    connect(tempo_edit, SIGNAL(escapePressed()), SIGNAL(escapePressed()));

    connect(tap_button, SIGNAL(clicked(bool)), SLOT(tap_tempo()));
    connect(&tap_timer, SIGNAL(timeout()), SLOT(tap_timer_signal()));
    tap_timer.stop();

    song_changed(-1);

    ensurePolished();
    buttonDefColor = tap_button->palette().color(QPalette::Button).name();
}

void TempoToolbar::pos_changed(int,unsigned,bool)
{
    song_changed(SC_TEMPO);
}

void TempoToolbar::song_changed(MusECore::SongChangedStruct_t type)
{
    if(type & (SC_TEMPO | SC_MASTER))
    {
        int tempo = MusEGlobal::tempomap.tempo(MusEGlobal::song->cpos());
        tempo_edit->blockSignals(true);
        tempo_edit->setValue(double(60000000.0/tempo));
        tempo_edit->blockSignals(false);
    }
    if(type & SC_MASTER)
    {
        setMasterTrack(MusEGlobal::tempomap.masterFlag());
    }
    if(type & SC_EXTERNAL_MIDI_SYNC)
    {
        syncChanged(MusEGlobal::extSyncFlag);
    }
}

void TempoToolbar::syncChanged(bool flag)
{
    tap_button->setEnabled(!flag);
    tempo_edit->setExternalMode(flag);
}

void TempoToolbar::tap_tempo()
{
    QDateTime local(QDateTime::currentDateTime());

    if (tap_timer.isActive()) {
        qint64 msecs_tap = last_tap_time.msecsTo(local);
        double t_tap = (double)60000.0f / (double)msecs_tap;
        tempo_edit->setValue(t_tap);
        emit tempo_edit->tempoChanged(t_tap);
    }

    tap_timer.start(2000);
    blinkButtonState = true;
    blink_timer->start(100);

    last_tap_time = local;
}

void TempoToolbar::tap_timer_signal()
{
    tap_timer.stop();
    blink_timer->stop();
    tap_button->setStyleSheet("QToolButton {background:" + buttonDefColor + "}");
}

void TempoToolbar::tapButtonBlink()
{
    blinkButtonState = !blinkButtonState;
    tap_button->setStyleSheet("QToolButton {background:" + (blinkButtonState ? buttonDefColor : "Crimson") + "}");
}

//---------------------------------------------------------
//   masterToggled
//---------------------------------------------------------

void TempoToolbar::masterToggled(bool val)
{
    emit masterTrackChanged(val);
}

bool TempoToolbar::masterTrack() const
{
    return _masterButton->isChecked();
}

void TempoToolbar::setMasterTrack(bool on)
{
    _masterButton->blockSignals(true);
    _masterButton->setChecked(on);
    _masterButton->blockSignals(false);
}



//---------------------------------
//   SigToolbar
//---------------------------------

SigToolbar::SigToolbar(QWidget* parent)
    : QToolBar(parent)
{
    init();
}

SigToolbar::SigToolbar(const QString& title, QWidget* parent)
    : QToolBar(title, parent)
{
    init();
}

void SigToolbar::init()
{
    setObjectName("Signature toolbar");
    sig_edit=new SigEdit(this);
    sig_edit->setContentsMargins(0, 0, 0, 0);
    sig_edit->setFocusPolicy(Qt::StrongFocus);
    sig_edit->setValue(MusECore::TimeSignature(4, 4));
    sig_edit->setToolTip(tr("Time signature at current position"));

    //  label=new QLabel(tr("Signature "),this);
    //  label->setContentsMargins(0, 0, 0, 0);

    //  addWidget(label);
    addWidget(sig_edit);

    connect(MusEGlobal::song, SIGNAL(songChanged(MusECore::SongChangedStruct_t)), this, SLOT(song_changed(MusECore::SongChangedStruct_t)));
    connect(MusEGlobal::song, SIGNAL(posChanged(int, unsigned, bool)), this, SLOT(pos_changed(int,unsigned,bool)));

    connect(sig_edit, SIGNAL(valueChanged(const MusECore::TimeSignature&)), MusEGlobal::song, SLOT(setSig(const MusECore::TimeSignature&)));
    connect(sig_edit, SIGNAL(returnPressed()), SIGNAL(returnPressed()));
    connect(sig_edit, SIGNAL(escapePressed()), SIGNAL(escapePressed()));

    song_changed(-1);
}


void SigToolbar::pos_changed(int,unsigned,bool)
{
    song_changed(SC_SIG);
}

void SigToolbar::song_changed(MusECore::SongChangedStruct_t type)
{
    if(type & SC_SIG)
    {
        int z, n;
        MusEGlobal::sigmap.timesig(MusEGlobal::song->cpos(), z, n);
        sig_edit->blockSignals(true);
        sig_edit->setValue(MusECore::TimeSignature(z, n));
        sig_edit->blockSignals(false);
    }
}


}  // namespace MusEGui
