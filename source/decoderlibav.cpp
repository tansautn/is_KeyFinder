/*************************************************************************

  Copyright 2011-2015 Ibrahim Sha'ath

  This file is part of KeyFinder.

  KeyFinder is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  KeyFinder is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with KeyFinder.  If not, see <http://www.gnu.org/licenses/>.

*************************************************************************/

#include "decoderlibav.h"

QMutex codecMutex;

AudioFileDecoder::AudioFileDecoder(const QString& filePath, const int maxDuration) : filePathCh(NULL), outputBuffer(NULL), outputBufferSamples(0), audioStream(-1), badPacketCount(0), badPacketThreshold(100), codec(NULL), fCtx(NULL), cCtx(NULL), swrCtx(NULL), packet(NULL), frame(NULL) {
  // convert filepath
#ifdef Q_OS_WIN
  const wchar_t* filePathWc = reinterpret_cast<const wchar_t*>(filePath.constData());
  filePathCh = qstrdup(utf16_to_utf8(filePathWc));
#else
  QByteArray encodedPath = QFile::encodeName(filePath);
  filePathCh = qstrdup(encodedPath.constData());
#endif

  QMutexLocker codecMutexLocker(&codecMutex); // mutex the libAV preparation

  // open file
  int openInputResult = avformat_open_input(&fCtx, filePathCh, NULL, NULL);
  if (openInputResult != 0) {
    qWarning("Could not open file %s (%d)", filePathCh, openInputResult);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotOpenFile(openInputResult).toUtf8().constData());
  }

  if (avformat_find_stream_info(fCtx, NULL) < 0) {
    qWarning("Could not find stream information for file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotFindStreamInformation().toUtf8().constData());
  }

  for (int i=0; i<(signed)fCtx->nb_streams; i++) {
    if (fCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audioStream = i;
      break;
    }
  }

  if (audioStream == -1) {
    qWarning("Could not find an audio stream for file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotFindAudioStream().toUtf8().constData());
  }

  // Determine duration
  int durationSeconds = fCtx->duration / AV_TIME_BASE;
  int durationMinutes = durationSeconds / 60;
  // First condition is a hack for bizarre overestimation of some MP3s
  if (durationMinutes < 720 && durationSeconds > maxDuration * 60) {
    qWarning("Duration of file %s (%d:%d) exceeds specified maximum (%d:00)", filePathCh, durationMinutes, durationSeconds % 60, maxDuration);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->durationExceedsPreference(durationMinutes, durationSeconds % 60, maxDuration).toUtf8().constData());
  }

  // Determine stream codec from its parameters (the AVStream::codec field was removed in ffmpeg 4.x)
  AVCodecParameters* codecPar = fCtx->streams[audioStream]->codecpar;
  codec = avcodec_find_decoder(codecPar->codec_id);
  if (codec == NULL) {
    qWarning("Audio stream has unsupported codec in file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavUnsupportedCodec().toUtf8().constData());
  }

  // Allocate a decoding context and copy the stream parameters into it
  cCtx = avcodec_alloc_context3(codec);
  if (cCtx == NULL) {
    qWarning("Could not allocate codec context for file %s", filePathCh);
    free();
    throw KeyFinder::Exception("Could not allocate codec context");
  }
  if (avcodec_parameters_to_context(cCtx, codecPar) < 0) {
    qWarning("Could not copy codec parameters for file %s", filePathCh);
    free();
    throw KeyFinder::Exception("Could not copy codec parameters to context");
  }

  // Open codec
  AVDictionary* dict = NULL;
  int codecOpenResult = avcodec_open2(cCtx, codec, &dict);
  if (codecOpenResult < 0) {
    qWarning("Could not open audio codec %s (%d) for file %s", codec->long_name, codecOpenResult, filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotOpenCodec(codec->long_name, codecOpenResult).toUtf8().constData());
  }

  // Some decoders only expose a bare channel count; synthesise a default layout so swresample is happy
  if (cCtx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && cCtx->ch_layout.nb_channels > 0) {
    av_channel_layout_default(&cCtx->ch_layout, cCtx->ch_layout.nb_channels);
  }

  // Resample everything to interleaved signed 16-bit at the source rate/layout (replaces the removed av_audio_resample API)
  int swrAllocResult = swr_alloc_set_opts2(
    &swrCtx,
    &cCtx->ch_layout, AV_SAMPLE_FMT_S16, cCtx->sample_rate,
    &cCtx->ch_layout, cCtx->sample_fmt,  cCtx->sample_rate,
    0, NULL);
  if (swrAllocResult < 0 || swrCtx == NULL || swr_init(swrCtx) < 0) {
    qWarning("Could not create SwrContext for file %s", filePathCh);
    free();
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotCreateResampleContext().toUtf8().constData());
  }

  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (packet == NULL || frame == NULL) {
    qWarning("Could not allocate packet/frame for file %s", filePathCh);
    free();
    throw KeyFinder::Exception("Could not allocate decoding packet or frame");
  }

  qDebug("Decoder prepared for %s (%s, %d)", filePathCh, av_get_sample_fmt_name(cCtx->sample_fmt), cCtx->sample_rate);
}

void AudioFileDecoder::free() {
  if (outputBuffer != NULL) av_free(outputBuffer);
  if (swrCtx != NULL) swr_free(&swrCtx);
  if (frame != NULL) av_frame_free(&frame);
  if (packet != NULL) av_packet_free(&packet);
  if (cCtx != NULL) avcodec_free_context(&cCtx);
  if (fCtx != NULL) avformat_close_input(&fCtx);
  if (filePathCh != NULL) delete[] filePathCh;
}

AudioFileDecoder::~AudioFileDecoder() {
  QMutexLocker codecMutexLocker(&codecMutex);
  free();
}

KeyFinder::AudioData* AudioFileDecoder::decodeNextAudioPacket() {
  KeyFinder::AudioData* audio = NULL;
  // Pull packets until a send produces decoded frames, or until the stream (and the decoder's internal buffer) is drained.
  while (audio == NULL) {
    int readResult = av_read_frame(fCtx, packet);
    if (readResult < 0) {
      // End of stream: flush the decoder once, then drain whatever frames remain buffered.
      avcodec_send_packet(cCtx, NULL);
      return receiveFrames(); // NULL once fully drained, which stops the caller's loop
    }
    if (packet->stream_index != audioStream) {
      av_packet_unref(packet);
      continue;
    }
    int sendResult = avcodec_send_packet(cCtx, packet);
    av_packet_unref(packet);
    if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
      badPacketCount++;
      if (badPacketCount > badPacketThreshold) {
        qWarning("Too many bad packets (%d) while decoding file %s", badPacketCount, filePathCh);
        throw KeyFinder::Exception(GuiStrings::getInstance()->libavTooManyBadPackets(badPacketThreshold).toUtf8().constData());
      }
      continue;
    }
    audio = receiveFrames();
  }
  return audio;
}

KeyFinder::AudioData* AudioFileDecoder::receiveFrames() {
  KeyFinder::AudioData* audio = NULL;
  while (true) {
    int recvResult = avcodec_receive_frame(cCtx, frame);
    if (recvResult == AVERROR(EAGAIN) || recvResult == AVERROR_EOF) break;
    if (recvResult < 0) {
      badPacketCount++;
      if (badPacketCount > badPacketThreshold) {
        qWarning("Too many bad packets (%d) while decoding file %s", badPacketCount, filePathCh);
        throw KeyFinder::Exception(GuiStrings::getInstance()->libavTooManyBadPackets(badPacketThreshold).toUtf8().constData());
      }
      break;
    }
    if (audio == NULL) {
      audio = new KeyFinder::AudioData();
      audio->setFrameRate((unsigned int) cCtx->sample_rate);
      audio->setChannels(cCtx->ch_layout.nb_channels);
    }
    try {
      appendFrameToAudio(frame, audio);
    } catch (...) {
      av_frame_unref(frame);
      throw;
    }
    av_frame_unref(frame);
  }
  return audio;
}

void AudioFileDecoder::appendFrameToAudio(AVFrame* f, KeyFinder::AudioData* audio) {
  int channels = cCtx->ch_layout.nb_channels;
  int maxOutSamplesPerChannel = swr_get_out_samples(swrCtx, f->nb_samples);
  ensureOutputBuffer(maxOutSamplesPerChannel * channels);

  uint8_t* outPtr = outputBuffer;
  int convertedPerChannel = swr_convert(swrCtx, &outPtr, maxOutSamplesPerChannel, (const uint8_t**)f->extended_data, f->nb_samples);
  if (convertedPerChannel < 0) {
    throw KeyFinder::Exception(GuiStrings::getInstance()->libavCouldNotResample().toUtf8().constData());
  }

  int newSamplesDecoded = convertedPerChannel * channels; // interleaved sample count
  int16_t* dataBuffer = (int16_t*)outputBuffer;
  int oldSampleCount = audio->getSampleCount();
  audio->addToSampleCount(newSamplesDecoded);
  audio->resetIterators();
  audio->advanceWriteIterator(oldSampleCount);
  for (int i = 0; i < newSamplesDecoded; i++) {
    audio->setSampleAtWriteIterator(static_cast<double>(dataBuffer[i]));
    audio->advanceWriteIterator();
  }
}

void AudioFileDecoder::ensureOutputBuffer(int samplesNeeded) {
  if (samplesNeeded <= outputBufferSamples) return;
  if (outputBuffer != NULL) av_free(outputBuffer);
  outputBuffer = (uint8_t*)av_malloc(samplesNeeded * sizeof(int16_t));
  outputBufferSamples = samplesNeeded;
}
