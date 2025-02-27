/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <QtGlobal>
#include <QObject>
#include <QIODevice>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QTextStream>
#include <QtDebug>

#include "core/logging.h"
#include "core/timeconstants.h"
#include "cueparser.h"
#include "playlistparsers/parserbase.h"

class CollectionBackendInterface;

const char *CueParser::kFileLineRegExp = "(\\S+)\\s+(?:\"([^\"]+)\"|(\\S+))\\s*(?:\"([^\"]+)\"|(\\S+))?";
const char *CueParser::kIndexRegExp = "(\\d{1,3}):(\\d{2}):(\\d{2})";

const char *CueParser::kPerformer = "performer";
const char *CueParser::kTitle = "title";
const char *CueParser::kSongWriter = "songwriter";
const char *CueParser::kFile = "file";
const char *CueParser::kTrack = "track";
const char *CueParser::kIndex = "index";
const char *CueParser::kAudioTrackType = "audio";
const char *CueParser::kRem = "rem";
const char *CueParser::kGenre = "genre";
const char *CueParser::kDate = "date";
const char *CueParser::kDisc = "discnumber";

CueParser::CueParser(CollectionBackendInterface *collection, QObject *parent)
    : ParserBase(collection, parent) {}

SongList CueParser::Load(QIODevice *device, const QString &playlist_path, const QDir &dir, const bool collection_search) const {

  SongList ret;

  QTextStream text_stream(device);

  QString dir_path = dir.absolutePath();
  // read the first line already
  QString line = text_stream.readLine();

  QList<CueEntry> entries;
  int files = 0;

  // -- whole file
  while (!text_stream.atEnd()) {

    QString album_artist;
    QString album;
    QString album_composer;
    QString file;
    QString file_type;
    QString album_genre;
    QString album_date;
    QString disc;

    // -- FILE section
    do {
      QStringList splitted = SplitCueLine(line);

      // uninteresting or incorrect line
      if (splitted.size() < 2) {
        continue;
      }
      const QString &line_name = splitted[0];
      const QString &line_value = splitted[1];

      if (line_name.compare(kPerformer, Qt::CaseInsensitive) == 0) {
        album_artist = line_value;
      }
      else if (line_name.compare(kTitle, Qt::CaseInsensitive) == 0) {
        album = line_value;
      }
      else if (line_name.compare(kSongWriter, Qt::CaseInsensitive) == 0) {
        album_composer = line_value;
      }
      else if (line_name.compare(kFile, Qt::CaseInsensitive) == 0) {
        file = QDir::isAbsolutePath(line_value) ? line_value : dir.absoluteFilePath(line_value);
        if (splitted.size() > 2) {
          file_type = splitted[2];
        }
      }
      else if (line_name.compare(kRem, Qt::CaseInsensitive) == 0) {
        if (splitted.size() < 3) {
          break;
        }
        if (line_value.compare(kGenre, Qt::CaseInsensitive) == 0) {
          album_genre = splitted[2];
        }
        else if (line_value.compare(kDate, Qt::CaseInsensitive) == 0) {
          album_date = splitted[2];
        }
        else if (line_value.compare(kDisc, Qt::CaseInsensitive) == 0) {
          disc = splitted[2];
        }
      }
      // end of the header -> go into the track mode
      else if (line_name.compare(kTrack, Qt::CaseInsensitive) == 0) {
        files++;
        break;
      }
      // just ignore the rest of possible field types for now...
    } while (!(line = text_stream.readLine()).isNull());

    if (line.isNull()) {
      qLog(Warning) << "the .cue file from " << dir_path << " defines no tracks!";
      return ret;
    }

    // if this is a data file, all of it's tracks will be ignored
    bool valid_file = file_type.compare("BINARY", Qt::CaseInsensitive) != 0 && file_type.compare("MOTOROLA", Qt::CaseInsensitive) != 0;

    QString track_type;
    QString index;
    QString artist;
    QString composer;
    QString title;
    QString date;
    QString genre;

    // TRACK section
    do {
      QStringList splitted = SplitCueLine(line);

      // uninteresting or incorrect line
      if (splitted.size() < 2) {
        continue;
      }

      const QString &line_name = splitted[0];
      const QString &line_value = splitted[1];
      QString line_additional = splitted.size() > 2 ? splitted[2].toLower() : "";

      if (line_name.compare(kTrack, Qt::CaseInsensitive) == 0) {

        // the beginning of another track's definition - we're saving the current one for later (if it's valid of course)
        // please note that the same code is repeated just after this 'do-while' loop
        if (valid_file && !index.isEmpty() && (track_type.isEmpty() || track_type.compare(kAudioTrackType, Qt::CaseInsensitive) == 0)) {
          entries.append(CueEntry(file, index, title, artist, album_artist, album, composer, album_composer, (genre.isEmpty() ? album_genre : genre), (date.isEmpty() ? album_date : date), disc));
        }

        // clear the state
        track_type = index = artist = composer = title = date = genre = "";

        if (!line_additional.isEmpty()) {
          track_type = line_additional;
        }

      }
      else if (line_name.compare(kIndex, Qt::CaseInsensitive) == 0) {

        // We need the index's position field
        if (!line_additional.isEmpty()) {

          // If there's none "01" index, we'll just take the first one also, we'll take the "01" index even if it's the last one
          if (line_value == "01" || index.isEmpty()) {

            index = line_additional;
          }
        }
      }
      else if (line_name.compare(kTitle, Qt::CaseInsensitive) == 0) {
        title = line_value;
      }
      else if (line_name.compare(kDate, Qt::CaseInsensitive) == 0) {
        date = line_value;
      }
      else if (line_name.compare(kPerformer, Qt::CaseInsensitive) == 0) {
        artist = line_value;
      }
      else if (line_name.compare(kSongWriter, Qt::CaseInsensitive) == 0) {
        composer = line_value;
        // End of track's for the current file -> parse next one
      }
      else if (line_name.compare(kRem, Qt::CaseInsensitive) == 0 && splitted.size() >= 3) {
        if (line_value.compare(kGenre, Qt::CaseInsensitive) == 0) {
          genre = splitted[2];
        }
        else if (line_value.compare(kDate, Qt::CaseInsensitive) == 0) {
          date = splitted[2];
        }
      }
      else if (line_name.compare(kFile, Qt::CaseInsensitive) == 0) {
        break;
      }

      // Just ignore the rest of possible field types for now...
    } while (!(line = text_stream.readLine()).isNull());

    // We didn't add the last song yet...
    if (valid_file && !index.isEmpty() && (track_type.isEmpty() || track_type.compare(kAudioTrackType, Qt::CaseInsensitive) == 0)) {
      entries.append(CueEntry(file, index, title, artist, album_artist, album, composer, album_composer, (genre.isEmpty() ? album_genre : genre), (date.isEmpty() ? album_date : date), disc));
    }
  }

  QDateTime cue_mtime = QFileInfo(playlist_path).lastModified();

  // Finalize parsing songs
  for (int i = 0; i < entries.length(); i++) {
    CueEntry entry = entries.at(i);

    Song song = LoadSong(entry.file, IndexToMarker(entry.index), dir, collection_search);

    // Cue song has mtime equal to qMax(media_file_mtime, cue_sheet_mtime)
    if (cue_mtime.isValid()) {
      song.set_mtime(qMax(cue_mtime.toSecsSinceEpoch(), song.mtime()));
    }
    song.set_cue_path(playlist_path);

    // Overwrite the stuff, we may have read from the file or collection, using the current .cue metadata

    // Set track number only in single-file mode
    if (files == 1) {
      song.set_track(i + 1);
    }

    // The last TRACK for every FILE gets it's 'end' marker from the media file's length
    if (i + 1 < entries.size() && entries.at(i).file == entries.at(i + 1).file) {
      // incorrect indices?
      if (!UpdateSong(entry, entries.at(i + 1).index, &song)) {
        continue;
      }
    }
    else {
      // incorrect index?
      if (!UpdateLastSong(entry, &song)) {
        continue;
      }
    }

    ret << song;
  }

  return ret;
}

// This and the kFileLineRegExp do most of the "dirty" work, namely: splitting the raw .cue
// line into logical parts and getting rid of all the unnecessary whitespaces and quoting.
QStringList CueParser::SplitCueLine(const QString &line) {

  QRegularExpression line_regexp(kFileLineRegExp);
  QRegularExpressionMatch re_match = line_regexp.match(line.trimmed());
  if (!re_match.hasMatch()) {
    return QStringList();
  }

  // Let's remove the empty entries while we're at it
  return re_match.capturedTexts().filter(QRegularExpression(".+")).mid(1, -1).replaceInStrings(QRegularExpression("^\"\"$"), "");

}

// Updates the song with data from the .cue entry. This one mustn't be used for the last song in the .cue file.
bool CueParser::UpdateSong(const CueEntry &entry, const QString &next_index, Song *song) {

  qint64 beginning = IndexToMarker(entry.index);
  qint64 end = IndexToMarker(next_index);

  // Incorrect indices (we won't be able to calculate beginning or end)
  if (beginning == -1 || end == -1) {
    return false;
  }

  // Believe the CUE: Init() forces validity
  song->Init(entry.title, entry.PrettyArtist(), entry.album, beginning, end);
  song->set_albumartist(entry.album_artist);
  song->set_composer(entry.PrettyComposer());
  song->set_genre(entry.genre);

  int year = entry.date.toInt();
  if (year > 0) song->set_year(year);
  int disc = entry.disc.toInt();
  if (disc > 0) song->set_disc(disc);

  return true;

}

// Updates the song with data from the .cue entry. This one must be used only for the last song in the .cue file.
bool CueParser::UpdateLastSong(const CueEntry &entry, Song *song) {

  qint64 beginning = IndexToMarker(entry.index);

  // Incorrect index (we won't be able to calculate beginning)
  if (beginning == -1) {
    return false;
  }

  // Believe the CUE and force validity (like UpdateSong() does)
  song->set_valid(true);

  song->set_title(entry.title);
  song->set_artist(entry.PrettyArtist());
  song->set_album(entry.album);
  song->set_albumartist(entry.album_artist);
  song->set_genre(entry.genre);
  song->set_composer(entry.PrettyComposer());

  int year = entry.date.toInt();
  if (year > 0) song->set_year(year);
  int disc = entry.disc.toInt();
  if (disc > 0) song->set_disc(disc);

  // We don't do anything with the end here because it's already set to the end of the media file (if it exists)
  song->set_beginning_nanosec(beginning);

  return true;

}

qint64 CueParser::IndexToMarker(const QString &index) {

  QRegularExpression index_regexp(kIndexRegExp);
  QRegularExpressionMatch re_match = index_regexp.match(index);
  if (!re_match.hasMatch()) {
    return -1;
  }

  QStringList splitted = re_match.capturedTexts().mid(1, -1);
  qint64 frames = splitted.at(0).toLongLong() * 60 * 75 + splitted.at(1).toLongLong() * 75 + splitted.at(2).toLongLong();
  return (frames * kNsecPerSec) / 75;

}

void CueParser::Save(const SongList &songs, QIODevice *device, const QDir &dir, Playlist::Path path_type) const {

  Q_UNUSED(songs);
  Q_UNUSED(device);
  Q_UNUSED(dir);
  Q_UNUSED(path_type);

  // TODO

}

// Looks for a track starting with one of the .cue's keywords.
bool CueParser::TryMagic(const QByteArray &data) const {

  QStringList splitted = QString::fromUtf8(data.constData()).split('\n');

  for (int i = 0; i < splitted.length(); i++) {
    QString line = splitted.at(i).trimmed();
    if (line.startsWith(kPerformer, Qt::CaseInsensitive) ||
        line.startsWith(kTitle, Qt::CaseInsensitive) ||
        line.startsWith(kFile, Qt::CaseInsensitive) ||
        line.startsWith(kTrack, Qt::CaseInsensitive)) {
      return true;
    }
  }

  return false;

}

QString CueParser::FindCueFilename(const QString &filename) {

  QStringList cue_files = QStringList() << filename + ".cue"
                                        << filename.section('.', 0, -2) + ".cue";

  for (const QString &cuefile : cue_files) {
    if (QFileInfo::exists(cuefile)) return cuefile;
  }

  return QString();

}
