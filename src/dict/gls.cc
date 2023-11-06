/* This file is (c) 2008-2017 Abs62
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include <zlib.h>
#include "gls.hh"
#include "iconv.hh"
#include "dictionary.hh"
#include "ufile.hh"
#include "btreeidx.hh"
#include "folding.hh"
#include "gddebug.hh"
#include "utf8.hh"
#include "wstring_qt.hh"
#include "chunkedstorage.hh"
#include "langcoder.hh"
#include "dictzip.hh"
#include "indexedzip.hh"
#include "ftshelpers.hh"

#include "htmlescape.hh"
#include "filetype.hh"
#include "tiff.hh"
#include "audiolink.hh"

#include <QString>
#include <QSemaphore>
#include <QThreadPool>
#include <QAtomicInt>
// For TIFF conversion
#include <QImage>
#include <QByteArray>
#include <QBuffer>

#include <QRegularExpression>
#if ( QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 ) )
  #include <QtCore5Compat/QTextCodec>
#else
  #include <QTextCodec>
#endif
#include <string>
#include <list>
#include <map>
#include <set>

#ifdef _MSC_VER
  #include <stub_msvc.h>
#endif

namespace Gls {

using std::list;
using std::map;
using std::set;
using std::multimap;
using std::pair;

using gd::wstring;
using gd::wchar;

using BtreeIndexing::WordArticleLink;
using BtreeIndexing::IndexedWords;
using BtreeIndexing::IndexInfo;
using Utf8::Encoding;
using Utf8::LineFeed;

/////////////// GlsScanner

class GlsScanner
{
  gzFile f;
  Encoding encoding;
  QTextCodec * codec;
  wstring dictionaryName;
  wstring dictionaryDecription, dictionaryAuthor;
  wstring langFrom, langTo;
  char readBuffer[ 10000 ];
  char * readBufferPtr;
  size_t readBufferLeft;
  LineFeed lineFeed;
  unsigned linesRead;

public:

  DEF_EX( Ex, "Gls scanner exception", Dictionary::Ex )
  DEF_EX_STR( exCantOpen, "Can't open .gls file", Ex )
  DEF_EX( exCantReadGlsFile, "Can't read .gls file", Ex )
  DEF_EX_STR( exMalformedGlsFile, "The .gls file is malformed:", Ex )
  DEF_EX( exEncodingError, "Encoding error", Ex ) // Should never happen really

  GlsScanner( string const & fileName );
  ~GlsScanner() noexcept;

  /// Returns the detected encoding of this file.
  Encoding getEncoding() const
  {
    return encoding;
  }

  /// Returns the dictionary's name, as was read from file's headers.
  wstring const & getDictionaryName() const
  {
    return dictionaryName;
  }

  /// Returns the dictionary's author, as was read from file's headers.
  wstring const & getDictionaryAuthor() const
  {
    return dictionaryAuthor;
  }

  /// Returns the dictionary's description, as was read from file's headers.
  wstring const & getDictionaryDescription() const
  {
    return dictionaryDecription;
  }

  /// Returns the dictionary's source language, as was read from file's headers.
  wstring const & getLangFrom() const
  {
    return langFrom;
  }

  /// Returns the dictionary's target language, as was read from file's headers.
  wstring const & getLangTo() const
  {
    return langTo;
  }

  /// Reads next line from the file. Returns true if reading succeeded --
  /// the string gets stored in the one passed, along with its physical
  /// file offset in the file (the uncompressed one if the file is compressed).
  /// If end of file is reached, false is returned.
  /// Reading begins from the first line after the headers (ones which end
  /// by the "### Glossary section:" line).
  bool readNextLine( wstring &, size_t & offset );
  /// Returns the number of lines read so far from the file.
  unsigned getLinesRead() const
  {
    return linesRead;
  }
};

GlsScanner::GlsScanner( string const & fileName ):
  encoding( Utf8::Utf8 ),
  readBufferPtr( readBuffer ),
  readBufferLeft( 0 ),
  linesRead( 0 )
{
  // Since .dz is backwards-compatible with .gz, we use gz- functions to
  // read it -- they are much nicer than the dict_data- ones.

  f = gd_gzopen( fileName.c_str() );
  if ( !f )
    throw exCantOpen( fileName );

  // Now try guessing the encoding by reading the first two bytes

  unsigned char firstBytes[ 2 ];

  if ( gzread( f, firstBytes, sizeof( firstBytes ) ) != sizeof( firstBytes ) ) {
    // Apparently the file's too short
    gzclose( f );
    throw exMalformedGlsFile( fileName );
  }

  // If the file begins with the dedicated Unicode marker, we just consume
  // it. If, on the other hand, it's not, we return the bytes back
  if ( firstBytes[ 0 ] == 0xFF && firstBytes[ 1 ] == 0xFE )
    encoding = Utf8::Utf16LE;
  else if ( firstBytes[ 0 ] == 0xFE && firstBytes[ 1 ] == 0xFF )
    encoding = Utf8::Utf16BE;
  else if ( firstBytes[ 0 ] == 0xEF && firstBytes[ 1 ] == 0xBB ) {
    // Looks like Utf8, read one more byte
    if ( gzread( f, firstBytes, 1 ) != 1 || firstBytes[ 0 ] != 0xBF ) {
      // Either the file's too short, or the BOM is weird
      gzclose( f );
      throw exMalformedGlsFile( fileName );
    }
    encoding = Utf8::Utf8;
  }
  else {
    if ( gzrewind( f ) ) {
      gzclose( f );
      throw exCantOpen( fileName );
    }
    encoding = Utf8::Utf8;
  }

  codec = QTextCodec::codecForName( Utf8::getEncodingNameFor( encoding ) );
  // We now can use our own readNextLine() function
  lineFeed = Utf8::initLineFeed( encoding );

  wstring str;
  wstring * currentField  = 0;
  wstring mark            = U"###";
  wstring titleMark       = U"### Glossary title:";
  wstring authorMark      = U"### Author:";
  wstring descriptionMark = U"### Description:";
  wstring langFromMark    = U"### Source language:";
  wstring langToMark      = U"### Target language:";
  wstring endOfHeaderMark = U"### Glossary section:";
  size_t offset;

  for ( ;; ) {
    if ( !readNextLine( str, offset ) ) {
      gzclose( f );
      throw exMalformedGlsFile( fileName );
    }

    if ( str.compare( 0, 3, mark.c_str(), 3 ) == 0 ) {
      currentField = 0;

      if ( str.compare( 0, titleMark.size(), titleMark ) == 0 ) {
        dictionaryName = wstring( str, titleMark.size(), str.size() - titleMark.size() );
        currentField   = &dictionaryName;
      }
      else if ( str.compare( 0, authorMark.size(), authorMark ) == 0 ) {
        dictionaryAuthor = wstring( str, authorMark.size(), str.size() - authorMark.size() );
        currentField     = &dictionaryAuthor;
      }
      else if ( str.compare( 0, descriptionMark.size(), descriptionMark ) == 0 ) {
        dictionaryDecription = wstring( str, descriptionMark.size(), str.size() - descriptionMark.size() );
        currentField         = &dictionaryDecription;
      }
      else if ( str.compare( 0, langFromMark.size(), langFromMark ) == 0 ) {
        langFrom = wstring( str, langFromMark.size(), str.size() - langFromMark.size() );
      }
      else if ( str.compare( 0, langToMark.size(), langToMark ) == 0 ) {
        langTo = wstring( str, langToMark.size(), str.size() - langToMark.size() );
      }
      else if ( str.compare( 0, endOfHeaderMark.size(), endOfHeaderMark ) == 0 ) {
        break;
      }
    }
    else {
      /// Handle multiline headers
      if ( currentField )
        *currentField += str;
    }
  }
}

bool GlsScanner::readNextLine( wstring & out, size_t & offset )
{
  offset = (size_t)( gztell( f ) - readBufferLeft );

  {
    // Check that we have bytes to read
    if ( readBufferLeft < 5000 ) {
      if ( !gzeof( f ) ) {
        // To avoid having to deal with ring logic, we move the remaining bytes
        // to the beginning
        memmove( readBuffer, readBufferPtr, readBufferLeft );

        // Read some more bytes to readBuffer
        int result = gzread( f, readBuffer + readBufferLeft, sizeof( readBuffer ) - readBufferLeft );

        if ( result == -1 )
          throw exCantReadGlsFile();

        readBufferPtr = readBuffer;
        readBufferLeft += (size_t)result;
      }
    }
    if ( readBufferLeft <= 0 )
      return false;

    int pos = Utf8::findFirstLinePosition( readBufferPtr, readBufferLeft, lineFeed.lineFeed, lineFeed.length );
    if ( pos == -1 )
      return false;
    QString line = codec->toUnicode( readBufferPtr, pos );

    line = Utils::rstrip( line );

    if ( pos > readBufferLeft ) {
      pos = readBufferLeft;
    }
    readBufferLeft -= pos;
    readBufferPtr += pos;
    linesRead++;

    out = line.toStdU32String();
    return true;
  }
}

GlsScanner::~GlsScanner() noexcept
{
  gzclose( f );
}

namespace {

////////////////// GLS Dictionary

using Dictionary::exCantReadFile;
DEF_EX( exUserAbort, "User abort", Dictionary::Ex )
DEF_EX_STR( exDictzipError, "DICTZIP error", Dictionary::Ex )

enum {
  Signature                = 0x58534c47, // GLSX on little-endian, XSLG on big-endian
  CurrentFormatVersion     = 1 + BtreeIndexing::FormatVersion + Folding::Version,
  CurrentZipSupportVersion = 2,
  CurrentFtsIndexVersion   = 1
};

struct IdxHeader
{
  uint32_t signature;             // First comes the signature, GLSX
  uint32_t formatVersion;         // File format version (CurrentFormatVersion)
  uint32_t zipSupportVersion;     // Zip support version -- narrows down reindexing
                                  // when it changes only for dictionaries with the
                                  // zip files
  int glsEncoding;                // Which encoding is used for the file indexed
  uint32_t chunksOffset;          // The offset to chunks' storage
  uint32_t indexBtreeMaxElements; // Two fields from IndexInfo
  uint32_t indexRootOffset;
  uint32_t articleCount;             // Number of articles this dictionary has
  uint32_t wordCount;                // Number of headwords this dictionary has
  uint32_t langFrom;                 // Source language
  uint32_t langTo;                   // Target language
  uint32_t hasZipFile;               // Non-zero means there's a zip file with resources
                                     // present
  uint32_t zipIndexBtreeMaxElements; // Two fields from IndexInfo of the zip
                                     // resource index.
  uint32_t zipIndexRootOffset;
}
#ifndef _MSC_VER
__attribute__( ( packed ) )
#endif
;

bool indexIsOldOrBad( string const & indexFile, bool hasZipFile )
{
  File::Class idx( indexFile, "rb" );

  IdxHeader header;

  return idx.readRecords( &header, sizeof( header ), 1 ) != 1 || header.signature != Signature
    || header.formatVersion != CurrentFormatVersion || (bool)header.hasZipFile != hasZipFile
    || ( hasZipFile && header.zipSupportVersion != CurrentZipSupportVersion );
}

class GlsDictionary: public BtreeIndexing::BtreeDictionary
{
  QMutex idxMutex;
  File::Class idx;
  IdxHeader idxHeader;
  dictData * dz;
  ChunkedStorage::Reader chunks;
  QMutex dzMutex;
  QMutex resourceZipMutex;
  IndexedZip resourceZip;

public:

  GlsDictionary( string const & id, string const & indexFile, vector< string > const & dictionaryFiles );

  ~GlsDictionary();

  string getName() noexcept override
  {
    return dictionaryName;
  }

  map< Dictionary::Property, string > getProperties() noexcept override
  {
    return map< Dictionary::Property, string >();
  }

  unsigned long getArticleCount() noexcept override
  {
    return idxHeader.articleCount;
  }

  unsigned long getWordCount() noexcept override
  {
    return idxHeader.wordCount;
  }

  inline quint32 getLangFrom() const override
  {
    return idxHeader.langFrom;
  }

  inline quint32 getLangTo() const override
  {
    return idxHeader.langTo;
  }

  sptr< Dictionary::WordSearchRequest > findHeadwordsForSynonym( wstring const & ) override;

  sptr< Dictionary::DataRequest >
  getArticle( wstring const &, vector< wstring > const & alts, wstring const &, bool ignoreDiacritics ) override;

  sptr< Dictionary::DataRequest > getResource( string const & name ) override;

  QString const & getDescription() override;

  QString getMainFilename() override;

  sptr< Dictionary::DataRequest >
  getSearchResults( QString const & searchString, int searchMode, bool matchCase, bool ignoreDiacritics ) override;

  void getArticleText( uint32_t articleAddress, QString & headword, QString & text ) override;

  void makeFTSIndex( QAtomicInt & isCancelled, bool firstIteration ) override;

  void setFTSParameters( Config::FullTextSearch const & fts ) override
  {
    can_FTS = enable_FTS && fts.enabled && !fts.disabledTypes.contains( "GLS", Qt::CaseInsensitive )
      && ( fts.maxDictionarySize == 0 || getArticleCount() <= fts.maxDictionarySize );
  }

protected:

  void loadIcon() noexcept override;

private:

  /// Loads the article, storing its headword and formatting the data it has
  /// into an html.
  void loadArticle( uint32_t address, string & headword, string & articleText );

  /// Loads the article
  void loadArticleText( uint32_t address, vector< string > & headwords, string & articleText );

  /// Process resource links (images, audios, etc)
  QString & filterResource( QString & article );

  friend class GlsResourceRequest;
  friend class GlsArticleRequest;
  friend class GlsHeadwordsRequest;
};

GlsDictionary::GlsDictionary( string const & id, string const & indexFile, vector< string > const & dictionaryFiles ):
  BtreeDictionary( id, dictionaryFiles ),
  idx( indexFile, "rb" ),
  idxHeader( idx.read< IdxHeader >() ),
  dz( 0 ),
  chunks( idx, idxHeader.chunksOffset )
{
  // Open the .gls file

  DZ_ERRORS error;
  dz = dict_data_open( getDictionaryFilenames()[ 0 ].c_str(), &error, 0 );

  if ( !dz )
    throw exDictzipError( string( dz_error_str( error ) ) + "(" + getDictionaryFilenames()[ 0 ] + ")" );

  // Read the dictionary name

  idx.seek( sizeof( idxHeader ) );

  vector< char > dName( idx.read< uint32_t >() );
  if ( dName.size() > 0 ) {
    idx.read( &dName.front(), dName.size() );
    dictionaryName = string( &dName.front(), dName.size() );
  }

  // Initialize the index

  openIndex( IndexInfo( idxHeader.indexBtreeMaxElements, idxHeader.indexRootOffset ), idx, idxMutex );

  // Open a resource zip file, if there's one

  if ( idxHeader.hasZipFile && ( idxHeader.zipIndexBtreeMaxElements || idxHeader.zipIndexRootOffset ) ) {
    resourceZip.openIndex( IndexInfo( idxHeader.zipIndexBtreeMaxElements, idxHeader.zipIndexRootOffset ),
                           idx,
                           idxMutex );

    QString zipName = QDir::fromNativeSeparators( getDictionaryFilenames().back().c_str() );

    if ( zipName.endsWith( ".zip", Qt::CaseInsensitive ) ) // Sanity check
      resourceZip.openZipFile( zipName );
  }

  // Full-text search parameters

  ftsIdxName = indexFile + Dictionary::getFtsSuffix();

}

GlsDictionary::~GlsDictionary()
{
  if ( dz )
    dict_data_close( dz );
}

void GlsDictionary::loadIcon() noexcept
{
  if ( dictionaryIconLoaded )
    return;

  QString fileName = QDir::fromNativeSeparators( getDictionaryFilenames()[ 0 ].c_str() );

  // Remove the extension
  if ( fileName.endsWith( ".gls.dz", Qt::CaseInsensitive ) )
    fileName.chop( 6 );
  else
    fileName.chop( 3 );

  if ( !loadIconFromFile( fileName ) ) {
    // Load failed -- use default icon
    dictionaryIcon = QIcon( ":/icons/icon32_gls.png" );
  }

  dictionaryIconLoaded = true;
}

QString const & GlsDictionary::getDescription()
{
  if ( !dictionaryDescription.isEmpty() )
    return dictionaryDescription;

  try {
    GlsScanner scanner( getDictionaryFilenames()[ 0 ] );
    string str = Utf8::encode( scanner.getDictionaryAuthor() );
    if ( !str.empty() )
      dictionaryDescription = QObject::tr( "Author: %1%2" ).arg( QString::fromUtf8( str.c_str() ) ).arg( "\n\n" );
    str = Utf8::encode( scanner.getDictionaryDescription() );
    if ( !str.empty() ) {
      QString desc = QString::fromUtf8( str.c_str() );
      desc.replace( "\t", "<br/>" );
      desc.replace( "\\n", "<br/>" );
      desc.replace( "<br>", "<br/>", Qt::CaseInsensitive );
      dictionaryDescription += Html::unescape( desc, Html::HtmlOption::Keep );
    }
  }
  catch ( std::exception & e ) {
    gdWarning( "GLS dictionary description reading failed: %s, error: %s\n", getName().c_str(), e.what() );
  }

  if ( dictionaryDescription.isEmpty() )
    dictionaryDescription = "NONE";

  return dictionaryDescription;
}

QString GlsDictionary::getMainFilename()
{
  return getDictionaryFilenames()[ 0 ].c_str();
}

void GlsDictionary::makeFTSIndex( QAtomicInt & isCancelled, bool firstIteration )
{
  if ( !( Dictionary::needToRebuildIndex( getDictionaryFilenames(), ftsIdxName )
          || FtsHelpers::ftsIndexIsOldOrBad( this ) ) )
    FTS_index_completed.ref();

  if ( haveFTSIndex() )
    return;

  if ( ensureInitDone().size() )
    return;

  if ( firstIteration && getArticleCount() > FTS::MaxDictionarySizeForFastSearch )
    return;

  gdDebug( "Gls: Building the full-text index for dictionary: %s\n", getName().c_str() );

  try {
    FtsHelpers::makeFTSIndex( this, isCancelled );
    FTS_index_completed.ref();
  }
  catch ( std::exception & ex ) {
    gdWarning( "Gls: Failed building full-text search index for \"%s\", reason: %s\n", getName().c_str(), ex.what() );
    QFile::remove( ftsIdxName.c_str() );
  }
}

void GlsDictionary::loadArticleText( uint32_t address, vector< string > & headwords, string & articleText )
{
  vector< char > chunk;
  char * articleProps;
  {
    QMutexLocker _( &idxMutex );

    articleProps = chunks.getBlock( address, chunk );
  }

  uint32_t articleOffset, articleSize;

  memcpy( &articleOffset, articleProps, sizeof( articleOffset ) );
  memcpy( &articleSize, articleProps + sizeof( articleOffset ), sizeof( articleSize ) );

  char * articleBody;

  {
    QMutexLocker _( &dzMutex );

    articleBody = dict_data_read_( dz, articleOffset, articleSize, 0, 0 );
  }

  headwords.clear();
  articleText.clear();
  string headword;

  if ( !articleBody ) {
    articleText = string( "\n\tDICTZIP error: " ) + dict_error_str( dz );
  }
  else {
    string articleData =
      Iconv::toUtf8( Utf8::getEncodingNameFor( Encoding( idxHeader.glsEncoding ) ), articleBody, articleSize );
    string::size_type start_pos = 0, end_pos = 0;

    for ( ;; ) {
      // Replace all "\r\n" by "\n"
      end_pos = articleData.find( "\r\n", start_pos );
      if ( end_pos == string::npos ) {
        articleText += articleData.substr( start_pos, end_pos );
        break;
      }
      else {
        articleText += articleData.substr( start_pos, end_pos - start_pos ) + "\n";
        start_pos = end_pos + 2;
      }
    }

    // Find headword
    start_pos = articleText.find( '\n' );
    if ( start_pos != string::npos ) {
      headword    = articleText.substr( 0, start_pos );
      articleText = articleText.substr( start_pos + 1, string::npos );
    }

    // Parse headwords

    start_pos = 0;
    end_pos   = 0;
    for ( ;; ) {
      end_pos = headword.find( '|', start_pos );
      if ( end_pos == wstring::npos ) {
        string hw = headword.substr( start_pos );
        if ( !hw.empty() )
          headwords.push_back( hw );
        break;
      }
      headwords.push_back( headword.substr( start_pos, end_pos - start_pos ) );
      start_pos = end_pos + 1;
    }
  }
}

void GlsDictionary::loadArticle( uint32_t address, string & headword, string & articleText )
{
  string articleBody;
  vector< string > headwords;
  loadArticleText( address, headwords, articleBody );

  QString article = QString::fromLatin1( "<div class=\"glsdict\">" );
  if ( headwords.size() ) {
    // Headwords
    article += "<div class=\"glsdict_headwords\"";
    if ( isFromLanguageRTL() )
      article += " dir=\"rtl\"";
    if ( headwords.size() > 1 ) {
      QString altHeadwords;
      for ( vector< string >::size_type i = 1; i < headwords.size(); i++ ) {
        if ( i > 1 )
          altHeadwords += ", ";
        altHeadwords += QString::fromUtf8( headwords[ i ].c_str(), headwords[ i ].size() );
      }
      article += " title=\"" + altHeadwords + "\"";
    }
    article += ">";

    headword = headwords.front();
    article += QString::fromUtf8( headword.c_str(), headword.size() );

    article += "</div>";
  }

  if ( isToLanguageRTL() )
    article += R"(<div style="display:inline;" dir="rtl">)";

  QString text = QString::fromUtf8( articleBody.c_str(), articleBody.size() );

  article += filterResource( text );

  if ( isToLanguageRTL() )
    article += "</div>";

  article += "</div>";

  articleText = string( article.toUtf8().data() );
}

QString & GlsDictionary::filterResource( QString & article )
{
  QRegularExpression imgRe( R"((<\s*img\s+[^>]*src\s*=\s*["']+)(?!(?:data|https?|ftp|qrcx):))",
                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::InvertedGreedinessOption );
  QRegularExpression linkRe( R"((<\s*link\s+[^>]*href\s*=\s*["']+)(?!(?:data|https?|ftp):))",
                             QRegularExpression::CaseInsensitiveOption | QRegularExpression::InvertedGreedinessOption );

  article.replace( imgRe, "\\1bres://" + QString::fromStdString( getId() ) + "/" )
    .replace( linkRe, "\\1bres://" + QString::fromStdString( getId() ) + "/" );

  // Handle links to articles

  QRegularExpression linksReg( R"(<a(\s+[^>]*)href\s*=\s*['"](bword://)?([^'"]+)['"])",
                               QRegularExpression::CaseInsensitiveOption );

  int pos = 0;
  QString articleNewText;
  QRegularExpressionMatchIterator it = linksReg.globalMatch( article );
  while ( it.hasNext() ) {
    QRegularExpressionMatch match = it.next();
    articleNewText += article.mid( pos, match.capturedStart() - pos );
    pos = match.capturedEnd();

    QString link = match.captured( 3 );

    if ( link.indexOf( ':' ) < 0 ) {
      QString newLink;
      if ( link.indexOf( '#' ) < 0 )
        newLink = QString( "<a" ) + match.captured( 1 ) + "href=\"bword:" + link + "\"";

      // Anchors

      if ( link.indexOf( '#' ) > 0 ) {
        newLink = QString( "<a" ) + match.captured( 1 ) + "href=\"gdlookup://localhost/" + link + "\"";

        newLink.replace( "#", "?gdanchor=" );
      }

      if ( !newLink.isEmpty() ) {
        articleNewText += newLink;
      }
      else
        articleNewText += match.captured();
    }
    else
      articleNewText += match.captured();
  }
  if ( pos ) {
    articleNewText += article.mid( pos );
    article = articleNewText;
    articleNewText.clear();
  }

  // Handle "audio" tags

  QRegularExpression audioRe( R"(<\s*audio\s+src\s*=\s*(["']+)([^"']+)(["'])\s*>(.*)</audio>)",
                              QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption
                                | QRegularExpression::InvertedGreedinessOption );


  pos = 0;

  it = audioRe.globalMatch( article );
  while ( it.hasNext() ) {
    QRegularExpressionMatch match = it.next();
    articleNewText += article.mid( pos, match.capturedStart() - pos );
    pos = match.capturedEnd();

    QString src = match.captured( 2 );

    if ( src.indexOf( "://" ) >= 0 )
      articleNewText += match.captured();
    else {
      std::string href = "\"gdau://" + getId() + "/" + src.toUtf8().data() + "\"";
      QString newTag   = QString::fromUtf8(
        ( addAudioLink( href, getId() ) + "<span class=\"gls_wav\"><a href=" + href + ">" ).c_str() );
      newTag += match.captured( 4 );
      if ( match.captured( 4 ).indexOf( "<img " ) < 0 )
        newTag += R"( <img src="qrc:///icons/playsound.png" border="0" alt="Play">)";
      newTag += "</a></span>";

      articleNewText += newTag;
    }
  }
  if ( pos ) {
    articleNewText += article.mid( pos );
    article = articleNewText;
    articleNewText.clear();
  }

  return article;
}

void GlsDictionary::getArticleText( uint32_t articleAddress, QString & headword, QString & text )
{
  try {
    vector< string > headwords;
    string articleStr;
    loadArticleText( articleAddress, headwords, articleStr );

    if ( !headwords.empty() )
      headword = QString::fromUtf8( headwords.front().data(), headwords.front().size() );

    text = Html::unescape( QString::fromStdString( articleStr ) );
  }
  catch ( std::exception & ex ) {
    gdWarning( "Gls: Failed retrieving article from \"%s\", reason: %s\n", getName().c_str(), ex.what() );
  }
}

/// GlsDictionary::findHeadwordsForSynonym()

class GlsHeadwordsRequest: public Dictionary::WordSearchRequest
{
  wstring word;
  GlsDictionary & dict;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  GlsHeadwordsRequest( wstring const & word_, GlsDictionary & dict_ ):
    word( word_ ),
    dict( dict_ )
  {
    f = QtConcurrent::run( [ this ]() {
      this->run();
    } );
  }

  void run();

  void cancel() override
  {
    isCancelled.ref();
  }

  ~GlsHeadwordsRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void GlsHeadwordsRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  try {
    vector< WordArticleLink > chain = dict.findArticles( word );

    wstring caseFolded = Folding::applySimpleCaseOnly( word );

    for ( auto & x : chain ) {
      if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
        finish();
        return;
      }

      string articleText;
      vector< string > headwords;

      dict.loadArticleText( x.articleOffset, headwords, articleText );

      wstring headwordDecoded = Utf8::decode( headwords.front() );

      if ( caseFolded != Folding::applySimpleCaseOnly( headwordDecoded ) ) {
        // The headword seems to differ from the input word, which makes the
        // input word its synonym.
        QMutexLocker _( &dataMutex );

        matches.push_back( headwordDecoded );
      }
    }
  }
  catch ( std::exception & e ) {
    setErrorString( QString::fromUtf8( e.what() ) );
  }

  finish();
}

sptr< Dictionary::WordSearchRequest > GlsDictionary::findHeadwordsForSynonym( wstring const & word )

{
  return synonymSearchEnabled ? std::make_shared< GlsHeadwordsRequest >( word, *this ) :
                                Class::findHeadwordsForSynonym( word );
}


/// GlsDictionary::getArticle()

class GlsArticleRequest: public Dictionary::DataRequest
{

  wstring word;
  vector< wstring > alts;
  GlsDictionary & dict;
  bool ignoreDiacritics;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  GlsArticleRequest( wstring const & word_,
                     vector< wstring > const & alts_,
                     GlsDictionary & dict_,
                     bool ignoreDiacritics_ ):
    word( word_ ),
    alts( alts_ ),
    dict( dict_ ),
    ignoreDiacritics( ignoreDiacritics_ )
  {
    f = QtConcurrent::run( [ this ]() {
      this->run();
    } );
  }

  void run();

  void cancel() override
  {
    isCancelled.ref();
  }

  ~GlsArticleRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void GlsArticleRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }
  try {
    vector< WordArticleLink > chain = dict.findArticles( word, ignoreDiacritics );

    for ( const auto & alt : alts ) {
      /// Make an additional query for each alt

      vector< WordArticleLink > altChain = dict.findArticles( alt, ignoreDiacritics );

      chain.insert( chain.end(), altChain.begin(), altChain.end() );
    }

    multimap< wstring, pair< string, string > > mainArticles, alternateArticles;

    set< uint32_t > articlesIncluded; // Some synonims make it that the articles
                                      // appear several times. We combat this
                                      // by only allowing them to appear once.

    wstring wordCaseFolded = Folding::applySimpleCaseOnly( word );
    if ( ignoreDiacritics )
      wordCaseFolded = Folding::applyDiacriticsOnly( wordCaseFolded );

    for ( auto & x : chain ) {
      if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
        finish();
        return;
      }

      if ( articlesIncluded.find( x.articleOffset ) != articlesIncluded.end() )
        continue; // We already have this article in the body.

      // Now grab that article

      string headword, articleText;

      dict.loadArticle( x.articleOffset, headword, articleText );

      // Ok. Now, does it go to main articles, or to alternate ones? We list
      // main ones first, and alternates after.

      // We do the case-folded comparison here.

      wstring headwordStripped = Folding::applySimpleCaseOnly( Utf8::decode( headword ) );
      if ( ignoreDiacritics )
        headwordStripped = Folding::applyDiacriticsOnly( headwordStripped );

      multimap< wstring, pair< string, string > > & mapToUse =
        ( wordCaseFolded == headwordStripped ) ? mainArticles : alternateArticles;

      mapToUse.insert(
        pair( Folding::applySimpleCaseOnly( Utf8::decode( headword ) ), pair( headword, articleText ) ) );

      articlesIncluded.insert( x.articleOffset );
    }

    if ( mainArticles.empty() && alternateArticles.empty() ) {
      // No such word
      finish();
      return;
    }

    string result;

    multimap< wstring, pair< string, string > >::const_iterator i;

    for ( i = mainArticles.begin(); i != mainArticles.end(); ++i ) {
      result += i->second.second;
    }

    for ( i = alternateArticles.begin(); i != alternateArticles.end(); ++i ) {
      result += i->second.second;
    }

    appendString( result );

    hasAnyData = true;
  }
  catch ( std::exception & e ) {
    setErrorString( QString::fromUtf8( e.what() ) );
  }

  finish();
}

sptr< Dictionary::DataRequest > GlsDictionary::getArticle( wstring const & word,
                                                           vector< wstring > const & alts,
                                                           wstring const &,
                                                           bool ignoreDiacritics )

{
  return std::make_shared< GlsArticleRequest >( word, alts, *this, ignoreDiacritics );
}

//////////////// GlsDictionary::getResource()

class GlsResourceRequest: public Dictionary::DataRequest
{

  GlsDictionary & dict;

  string resourceName;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  GlsResourceRequest( GlsDictionary & dict_, string const & resourceName_ ):
    dict( dict_ ),
    resourceName( resourceName_ )
  {
    f = QtConcurrent::run( [ this ]() {
      this->run();
    } );
  }

  void run();

  void cancel() override
  {
    isCancelled.ref();
  }

  ~GlsResourceRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void GlsResourceRequest::run()
{
  // Some runnables linger enough that they are cancelled before they start
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  try {
    string n = dict.getContainingFolder().toStdString() + Utils::Fs::separator() + resourceName;

    GD_DPRINTF( "n is %s\n", n.c_str() );

    try {
      QMutexLocker _( &dataMutex );

      File::loadFromFile( n, data );
    }
    catch ( File::exCantOpen & ) {
      n = dict.getDictionaryFilenames()[ 0 ] + ".files" + Utils::Fs::separator() + resourceName;

      try {
        QMutexLocker _( &dataMutex );

        File::loadFromFile( n, data );
      }
      catch ( File::exCantOpen & ) {
        // Try reading from zip file

        if ( dict.resourceZip.isOpen() ) {
          QMutexLocker _( &dataMutex );

          if ( !dict.resourceZip.loadFile( Utf8::decode( resourceName ), data ) )
            throw; // Make it fail since we couldn't read the archive
        }
        else
          throw;
      }
    }

    if ( Filetype::isNameOfTiff( resourceName ) ) {
      // Convert it

      QMutexLocker _( &dataMutex );
      GdTiff::tiff2img( data );
    }

    if ( Filetype::isNameOfCSS( resourceName ) ) {
      QMutexLocker _( &dataMutex );

      QString css = QString::fromUtf8( data.data(), data.size() );

      // Correct some url's

      QString id = QString::fromUtf8( dict.getId().c_str() );
      int pos    = 0;

      QRegularExpression links( R"(url\(\s*(['"]?)([^'"]*)(['"]?)\s*\))", QRegularExpression::CaseInsensitiveOption );

      QString newCSS;
      QRegularExpressionMatchIterator it = links.globalMatch( css );
      while ( it.hasNext() ) {
        QRegularExpressionMatch match = it.next();
        newCSS += css.mid( pos, match.capturedStart() - pos );
        pos = match.capturedEnd();

        QString url = match.captured( 2 );

        if ( url.indexOf( ":/" ) >= 0 || url.indexOf( "data:" ) >= 0 ) {
          // External link
          newCSS += match.captured();
          continue;
        }

        QString newUrl =
          QString( "url(" ) + match.captured( 1 ) + "bres://" + id + "/" + url + match.captured( 3 ) + ")";
        newCSS += newUrl;
      }
      if ( pos ) {
        newCSS += css.mid( pos );
        css = newCSS;
        newCSS.clear();
      }

      dict.isolateCSS( css );
      QByteArray bytes = css.toUtf8();
      data.resize( bytes.size() );
      memcpy( &data.front(), bytes.constData(), bytes.size() );
    }

    QMutexLocker _( &dataMutex );
    hasAnyData = true;
  }
  catch ( std::exception & ex ) {
    gdWarning( "GLS: Failed loading resource \"%s\" for \"%s\", reason: %s\n",
               resourceName.c_str(),
               dict.getName().c_str(),
               ex.what() );
    // Resource not loaded -- we don't set the hasAnyData flag then
  }

  finish();
}

sptr< Dictionary::DataRequest > GlsDictionary::getResource( string const & name )

{
  return std::make_shared< GlsResourceRequest >( *this, name );
}

sptr< Dictionary::DataRequest > GlsDictionary::getSearchResults( QString const & searchString,
                                                                 int searchMode,
                                                                 bool matchCase,

                                                                 bool ignoreDiacritics )
{
  return std::make_shared< FtsHelpers::FTSResultsRequest >( *this,
                                                            searchString,
                                                            searchMode,
                                                            matchCase,
                                                            ignoreDiacritics );
}

} // anonymous namespace

/// makeDictionaries

vector< sptr< Dictionary::Class > > makeDictionaries( vector< string > const & fileNames,
                                                      string const & indicesDir,
                                                      Dictionary::Initializing & initializing )

{
  vector< sptr< Dictionary::Class > > dictionaries;

  for ( const auto & fileName : fileNames ) {
    // Try .gls and .gls.dz suffixes

    if ( !Utils::endsWithIgnoreCase( fileName, ".gls" ) && !Utils::endsWithIgnoreCase( fileName, ".gls.dz" ) )
      continue;

    unsigned atLine = 0; // Indicates current line in .gls, for debug purposes

    try {
      vector< string > dictFiles( 1, fileName );

      string dictId = Dictionary::makeDictionaryId( dictFiles );

      // See if there's a zip file with resources present. If so, include it.

      string baseName = ( fileName[ fileName.size() - 4 ] == '.' ) ? string( fileName, 0, fileName.size() - 4 ) :
                                                                     string( fileName, 0, fileName.size() - 7 );

      string zipFileName;

      if ( File::tryPossibleZipName( baseName + ".gls.files.zip", zipFileName )
           || File::tryPossibleZipName( baseName + ".gls.dz.files.zip", zipFileName )
           || File::tryPossibleZipName( baseName + ".GLS.FILES.ZIP", zipFileName )
           || File::tryPossibleZipName( baseName + ".GLS.DZ.FILES.ZIP", zipFileName ) )
        dictFiles.push_back( zipFileName );

      string indexFile = indicesDir + dictId;

      if ( Dictionary::needToRebuildIndex( dictFiles, indexFile )
           || indexIsOldOrBad( indexFile, zipFileName.size() ) ) {
        GlsScanner scanner( fileName );

        try { // Here we intercept any errors during the read to save line at
              // which the incident happened. We need alive scanner for that.

          // Building the index
          initializing.indexingDictionary( Utf8::encode( scanner.getDictionaryName() ) );

          gdDebug( "Gls: Building the index for dictionary: %s\n",
                   QString::fromStdU32String( scanner.getDictionaryName() ).toUtf8().data() );

          File::Class idx( indexFile, "wb" );

          IdxHeader idxHeader;

          memset( &idxHeader, 0, sizeof( idxHeader ) );

          // We write a dummy header first. At the end of the process the header
          // will be rewritten with the right values.

          idx.write( idxHeader );

          string dictionaryName = Utf8::encode( scanner.getDictionaryName() );

          idx.write( (uint32_t)dictionaryName.size() );
          idx.write( dictionaryName.data(), dictionaryName.size() );

          idxHeader.glsEncoding = scanner.getEncoding();

          IndexedWords indexedWords;

          ChunkedStorage::Writer chunks( idx );

          wstring curString;
          size_t curOffset;

          uint32_t articleCount = 0, wordCount = 0;

          for ( ;; ) {
            // Find the headwords

            if ( !scanner.readNextLine( curString, curOffset ) )
              break; // Clean end of file

            if ( curString.empty() )
              continue;

            uint32_t articleOffset = curOffset;

            // Parse headwords

            list< wstring > allEntryWords;
            wstring::size_type start_pos = 0, end_pos = 0;
            for ( ;; ) {
              end_pos = curString.find( '|', start_pos );
              if ( end_pos == wstring::npos ) {
                wstring headword = curString.substr( start_pos );
                if ( !headword.empty() )
                  allEntryWords.push_back( headword );
                break;
              }
              allEntryWords.push_back( curString.substr( start_pos, end_pos - start_pos ) );
              start_pos = end_pos + 1;
            }

            // Skip article body

            for ( ;; ) {
              if ( !scanner.readNextLine( curString, curOffset ) )
                break;
              if ( curString.empty() )
                break;
            }

            // Insert new entry

            uint32_t descOffset = chunks.startNewBlock();
            chunks.addToBlock( &articleOffset, sizeof( articleOffset ) );

            uint32_t articleSize = curOffset - articleOffset;
            chunks.addToBlock( &articleSize, sizeof( articleSize ) );

            for ( auto & allEntryWord : allEntryWords )
              indexedWords.addWord( allEntryWord, descOffset );

            ++articleCount;
            wordCount += allEntryWords.size();
          }

          // Finish with the chunks

          idxHeader.chunksOffset = chunks.finish();

          // Build index

          IndexInfo idxInfo = BtreeIndexing::buildIndex( indexedWords, idx );

          idxHeader.indexBtreeMaxElements = idxInfo.btreeMaxElements;
          idxHeader.indexRootOffset       = idxInfo.rootOffset;

          indexedWords.clear(); // Release memory -- no need for this data

          // If there was a zip file, index it too

          if ( zipFileName.size() ) {
            GD_DPRINTF( "Indexing zip file\n" );

            idxHeader.hasZipFile = 1;

            IndexedWords zipFileNames;
            IndexedZip zipFile;
            if ( zipFile.openZipFile( QDir::fromNativeSeparators( zipFileName.c_str() ) ) )
              zipFile.indexFile( zipFileNames );

            if ( !zipFileNames.empty() ) {
              // Build the resulting zip file index

              IndexInfo idxInfo = BtreeIndexing::buildIndex( zipFileNames, idx );

              idxHeader.zipIndexBtreeMaxElements = idxInfo.btreeMaxElements;
              idxHeader.zipIndexRootOffset       = idxInfo.rootOffset;
            }
            else {
              // Bad zip file -- no index (though the mark that we have one
              // remains)
              idxHeader.zipIndexBtreeMaxElements = 0;
              idxHeader.zipIndexRootOffset       = 0;
            }
          }
          else
            idxHeader.hasZipFile = 0;

          // That concludes it. Update the header.

          idxHeader.signature         = Signature;
          idxHeader.formatVersion     = CurrentFormatVersion;
          idxHeader.zipSupportVersion = CurrentZipSupportVersion;

          idxHeader.articleCount = articleCount;
          idxHeader.wordCount    = wordCount;

          idxHeader.langFrom = LangCoder::findIdForLanguage( scanner.getLangFrom() );
          idxHeader.langTo   = LangCoder::findIdForLanguage( scanner.getLangTo() );
          if ( idxHeader.langFrom == 0 && idxHeader.langTo == 0 ) {
            // if no languages found, try dictionary's file name
            QPair< quint32, quint32 > langs = LangCoder::findIdsForFilename( QString::fromStdString( dictFiles[ 0 ] ) );

            // if no languages found, try dictionary's name
            if ( langs.first == 0 || langs.second == 0 ) {
              langs = LangCoder::findIdsForFilename( QString::fromStdString( dictionaryName ) );
            }
            idxHeader.langFrom = langs.first;
            idxHeader.langTo   = langs.second;
          }

          idx.rewind();

          idx.write( &idxHeader, sizeof( idxHeader ) );
        } // In-place try for saving line count
        catch ( ... ) {
          atLine = scanner.getLinesRead();
          throw;
        }

      } // if need to rebuild
      dictionaries.push_back( std::make_shared< GlsDictionary >( dictId, indexFile, dictFiles ) );
    }
    catch ( std::exception & e ) {
      gdWarning( "GLS dictionary reading failed: %s:%u, error: %s\n", fileName.c_str(), atLine, e.what() );
    }
  }

  return dictionaries;
}

} // namespace Gls
