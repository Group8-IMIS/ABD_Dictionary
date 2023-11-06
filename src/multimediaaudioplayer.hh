/* This file is (c) 2018 Igor Kushnir <igorkuo@gmail.com>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef MULTIMEDIAAUDIOPLAYER_HH_INCLUDED
#define MULTIMEDIAAUDIOPLAYER_HH_INCLUDED

#ifdef MAKE_QTMULTIMEDIA_PLAYER

  #include <QBuffer>
  #include <QMediaPlayer>
  #include "audioplayerinterface.hh"
  #if ( QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 ) )
    #include <QAudioOutput>
  #endif
  #if ( QT_VERSION >= QT_VERSION_CHECK( 6, 2, 0 ) )
    #include <QMediaDevices>
  #endif
  #include <QPointer>

class MultimediaAudioPlayer: public AudioPlayerInterface
{
  Q_OBJECT

public:
  MultimediaAudioPlayer();

  virtual QString play( const char * data, int size );
  virtual void stop();

private slots:
  void onMediaPlayerError();
  void audioOutputChange();


private:
  QPointer< QBuffer > audioBuffer;
  QMediaPlayer player; ///< Depends on audioBuffer.
  #if ( QT_VERSION >= QT_VERSION_CHECK( 6, 2, 0 ) )
  QAudioOutput audioOutput;
  #endif
  #if ( QT_VERSION >= QT_VERSION_CHECK( 6, 2, 0 ) )
  QMediaDevices mediaDevices;
  #endif
};

#endif // MAKE_QTMULTIMEDIA_PLAYER

#endif // MULTIMEDIAAUDIOPLAYER_HH_INCLUDED
