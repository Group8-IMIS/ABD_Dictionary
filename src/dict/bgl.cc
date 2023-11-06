/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "bgl.hh"
#include "bgl_babylon.hh"
#include "btreeidx.hh"
#include "chunkedstorage.hh"
#include "file.hh"
#include "folding.hh"
#include "ftshelpers.hh"
#include "gddebug.hh"
#include "htmlescape.hh"
#include "langcoder.hh"
#include "language.hh"
#include "utf8.hh"
#include "utils.hh"

#include <ctype.h>
#include <list>
#include <map>
#include <set>
#include <string.h>
#include <zlib.h>

#ifdef _MSC_VER
  #include <stub_msvc.h>
#endif

#include <QAtomicInt>
#include <QPainter>
#include <QRegularExpression>
#include <QSemaphore>
#include <QThreadPool>

#if ( QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 ) )
  #include <QtCore5Compat/QRegExp>
#else
  #include <QRegExp>
#endif

namespace Bgl {

using std::map;
using std::multimap;
using std::set;
using gd::wstring;
using gd::wchar;
using std::list;
using std::pair;
using std::string;

using BtreeIndexing::WordArticleLink;
using BtreeIndexing::IndexedWords;
using BtreeIndexing::IndexInfo;

namespace {
enum {
  Signature            = 0x584c4742, // BGLX on little-endian, XLGB on big-endian
  CurrentFormatVersion = 19 + BtreeIndexing::FormatVersion
};

struct IdxHeader
{
  uint32_t signature;      // First comes the signature, BGLX
  uint32_t formatVersion;  // File format version, currently 1.
  uint32_t parserVersion;  // Version of the parser used to parse the BGL file.
                           // If it's lower than the current one, the file is to
                           // be re-parsed.
  uint32_t foldingVersion; // Version of the folding algorithm used when building
                           // index. If it's different from the current one,
                           // the file is to be rebuilt.
  uint32_t articleCount;   // Total number of articles, for informative purposes only
  uint32_t wordCount;      // Total number of words, for informative purposes only
  /// Add more fields here, like name, description, author and such.
  uint32_t chunksOffset;          // The offset to chunks' storage
  uint32_t indexBtreeMaxElements; // Two fields from IndexInfo
  uint32_t indexRootOffset;
  uint32_t resourceListOffset; // The offset of the list of resources
  uint32_t resourcesCount;     // Number of resources stored
  uint32_t langFrom;           // Source language
  uint32_t langTo;             // Target language
  uint32_t iconAddress;        // Address of the icon in the chunks' storage
  uint32_t iconSize;           // Size of the icon in the chunks' storage, 0 = no icon
  uint32_t descriptionAddress; // Address of the dictionary description in the chunks' storage
  uint32_t descriptionSize;    // Size of the description in the chunks' storage, 0 = no description
}
#ifndef _MSC_VER
__attribute__( ( packed ) )
#endif
;

bool indexIsOldOrBad( string const & indexFile )
{
  File::Class idx( indexFile, "rb" );

  IdxHeader header;

  return idx.readRecords( &header, sizeof( header ), 1 ) != 1 || header.signature != Signature
    || header.formatVersion != CurrentFormatVersion || header.parserVersion != Babylon::ParserVersion
    || header.foldingVersion != Folding::Version;
}

// Removes the $1$-like postfix
string removePostfix( string const & in )
{
  if ( in.size() && in[ in.size() - 1 ] == '$' ) {
    // Find the end of it and cut it, barring any unexpectedness
    for ( long x = in.size() - 2; x >= 0; x-- ) {
      if ( in[ x ] == '$' )
        return in.substr( 0, x );
      else if ( !isdigit( in[ x ] ) )
        break;
    }
  }

  return in;
}

// Removes any leading or trailing whitespace
void trimWs( string & word )
{
  if ( word.size() ) {
    unsigned begin = 0;

    while ( begin < word.size() && Utf8::isspace( word[ begin ] ) )
      ++begin;

    if ( begin == word.size() ) // Consists of ws entirely?
      word.clear();
    else {
      unsigned end = word.size();

      // Doesn't consist of ws entirely, so must end with just isspace()
      // condition.
      while ( Utf8::isspace( word[ end - 1 ] ) )
        --end;

      if ( end != word.size() || begin )
        word = string( word, begin, end - begin );
    }
  }
}

void addEntryToIndex( string & word,
                      uint32_t articleOffset,
                      IndexedWords & indexedWords,
                      vector< wchar > & wcharBuffer )
{
  // Strip any leading or trailing whitespaces
  trimWs( word );

  // If the word starts with a slash, we drop it. There are quite a lot
  // of them, and they all seem to be redudant duplicates.

  if ( word.size() && word[ 0 ] == '/' )
    return;

  // Check the input word for a superscript postfix ($1$, $2$ etc), which
  // signifies different meaning in Bgl files. We emit different meaning
  // as different articles, but they appear in the index as the same word.

  if ( word.size() && word[ word.size() - 1 ] == '$' ) {
    word = removePostfix( word );
    trimWs( word );
  }

  // Convert the word from utf8 to wide chars
  indexedWords.addWord( Utf8::decode( word ), articleOffset );
}


DEF_EX( exFailedToDecompressArticle, "Failed to decompress article's body", Dictionary::Ex )
DEF_EX( exChunkIndexOutOfRange, "Chunk index is out of range", Dictionary::Ex )

class BglDictionary: public BtreeIndexing::BtreeDictionary
{
  QMutex idxMutex;
  File::Class idx;
  IdxHeader idxHeader;
  ChunkedStorage::Reader chunks;

public:

  BglDictionary( string const & id, string const & indexFile, string const & dictionaryFile );

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

  sptr< Dictionary::DataRequest >
  getSearchResults( QString const & searchString, int searchMode, bool matchCase, bool ignoreDiacritics ) override;
  QString const & getDescription() override;

  void getArticleText( uint32_t articleAddress, QString & headword, QString & text ) override;

  void makeFTSIndex( QAtomicInt & isCancelled, bool firstIteration ) override;

  void setFTSParameters( Config::FullTextSearch const & fts ) override
  {
    can_FTS = enable_FTS && fts.enabled && !fts.disabledTypes.contains( "BGL", Qt::CaseInsensitive )
      && ( fts.maxDictionarySize == 0 || getArticleCount() <= fts.maxDictionarySize );
  }

protected:

  void loadIcon() noexcept override;

private:


  /// Loads an article with the given offset, filling the given strings.
  void loadArticle( uint32_t offset, string & headword, string & displayedHeadword, string & articleText );

  static void replaceCharsetEntities( string & );

  friend class BglHeadwordsRequest;
  friend class BglArticleRequest;
  friend class BglResourceRequest;
};

BglDictionary::BglDictionary( string const & id, string const & indexFile, string const & dictionaryFile ):
  BtreeDictionary( id, vector< string >( 1, dictionaryFile ) ),
  idx( indexFile, "rb" ),
  idxHeader( idx.read< IdxHeader >() ),
  chunks( idx, idxHeader.chunksOffset )
{
  idx.seek( sizeof( idxHeader ) );

  // Read the dictionary's name

  size_t len = idx.read< uint32_t >();

  if ( len ) {
    vector< char > nameBuf( len );

    idx.read( &nameBuf.front(), len );

    dictionaryName = string( &nameBuf.front(), len );
  }

  // Initialize the index

  openIndex( IndexInfo( idxHeader.indexBtreeMaxElements, idxHeader.indexRootOffset ), idx, idxMutex );

  ftsIdxName = indexFile + Dictionary::getFtsSuffix();
}

void BglDictionary::loadIcon() noexcept
{
  if ( dictionaryIconLoaded )
    return;

  QString fileName = QDir::fromNativeSeparators( QString::fromStdString( getDictionaryFilenames()[ 0 ] ) );

  // Remove the extension
  fileName.chop( 3 );

  if ( !loadIconFromFile( fileName ) ) {
    if ( idxHeader.iconSize ) {

      // Try loading icon now

      vector< char > chunk;

      QMutexLocker _( &idxMutex );

      char * iconData = chunks.getBlock( idxHeader.iconAddress, chunk );

      QImage img;

      if ( img.loadFromData( (unsigned char *)iconData, idxHeader.iconSize ) ) {

        // Transform it to be square
        int max = img.width() > img.height() ? img.width() : img.height();

        QImage result( max, max, QImage::Format_ARGB32 );
        result.fill( 0 ); // Black transparent

        QPainter painter( &result );
        painter.setRenderHint( QPainter::RenderHint::Antialiasing );

        painter.drawImage( QPoint( img.width() == max ? 0 : ( max - img.width() ) / 2,
                                   img.height() == max ? 0 : ( max - img.height() ) / 2 ),
                           img );

        painter.end();

        dictionaryIcon = QIcon( QPixmap::fromImage( result ) );
      }
    }

    if ( dictionaryIcon.isNull() )
      dictionaryIcon = QIcon( ":/icons/icon32_bgl.png" );
  }

  dictionaryIconLoaded = true;
}

void BglDictionary::loadArticle( uint32_t offset, string & headword, string & displayedHeadword, string & articleText )
{
  vector< char > chunk;

  QMutexLocker _( &idxMutex );

  char * articleData = chunks.getBlock( offset, chunk );

  headword = articleData;

  displayedHeadword = articleData + headword.size() + 1;

  articleText = string( articleData + headword.size() + displayedHeadword.size() + 2 );
}

QString const & BglDictionary::getDescription()
{
  if ( !dictionaryDescription.isEmpty() )
    return dictionaryDescription;

  if ( idxHeader.descriptionSize == 0 )
    dictionaryDescription = "NONE";
  else {
    QMutexLocker _( &idxMutex );
    vector< char > chunk;
    char * dictDescription = chunks.getBlock( idxHeader.descriptionAddress, chunk );
    string str( dictDescription );
    if ( !str.empty() )
      dictionaryDescription += QObject::tr( "Copyright: %1%2" )
                                 .arg( Html::unescape( QString::fromUtf8( str.data(), str.size() ) ) )
                                 .arg( "\n\n" );
    dictDescription += str.size() + 1;

    str = string( dictDescription );
    if ( !str.empty() )
      dictionaryDescription +=
        QObject::tr( "Author: %1%2" ).arg( QString::fromUtf8( str.data(), str.size() ) ).arg( "\n\n" );
    dictDescription += str.size() + 1;

    str = string( dictDescription );
    if ( !str.empty() )
      dictionaryDescription +=
        QObject::tr( "E-mail: %1%2" ).arg( QString::fromUtf8( str.data(), str.size() ) ).arg( "\n\n" );
    dictDescription += str.size() + 1;

    str = string( dictDescription );
    if ( !str.empty() )
      dictionaryDescription += Html::unescape( QString::fromUtf8( str.data(), str.size() ) );
  }

  return dictionaryDescription;
}

void BglDictionary::getArticleText( uint32_t articleAddress, QString & headword, QString & text )
{
  try {
    string headwordStr, displayedHeadwordStr, articleStr;
    loadArticle( articleAddress, headwordStr, displayedHeadwordStr, articleStr );

    // Some headword normalization similar while indexing
    trimWs( headwordStr );

    if ( headwordStr.size() && headwordStr[ 0 ] == '/' )
      headwordStr.erase(); // We will take headword from index later

    if ( headwordStr.size() && headwordStr[ headwordStr.size() - 1 ] == '$' ) {
      headwordStr = removePostfix( headwordStr );
      trimWs( headwordStr );
    }

    headword = QString::fromUtf8( headwordStr.data(), headwordStr.size() );

    wstring wstr = Utf8::decode( articleStr );

    if ( getLangTo() == LangCoder::code2toInt( "he" ) ) {
      for ( char32_t & i : wstr ) {
        if (
          ( i >= 224 && i <= 250 )
          || ( i >= 192
               && i
                 <= 210 ) ) // Hebrew chars encoded ecoded as windows-1255 or ISO-8859-8, or as vowel-points of windows-1255
          i += 1488 - 224; // Convert to Hebrew unicode
      }
    }

    text = Html::unescape( QString::fromStdU32String( wstr ) );
  }
  catch ( std::exception & ex ) {
    gdWarning( "BGL: Failed retrieving article from \"%s\", reason: %s\n", getName().c_str(), ex.what() );
  }
}

void BglDictionary::makeFTSIndex( QAtomicInt & isCancelled, bool firstIteration )
{
  if ( !( Dictionary::needToRebuildIndex( getDictionaryFilenames(), ftsIdxName )
          || FtsHelpers::ftsIndexIsOldOrBad( this ) ) )
    FTS_index_completed.ref();

  if ( haveFTSIndex() )
    return;

  if ( firstIteration && getArticleCount() > FTS::MaxDictionarySizeForFastSearch )
    return;

  gdDebug( "Bgl: Building the full-text index for dictionary: %s\n", getName().c_str() );

  try {
    FtsHelpers::makeFTSIndex( this, isCancelled );
    FTS_index_completed.ref();
  }
  catch ( std::exception & ex ) {
    gdWarning( "Bgl: Failed building full-text search index for \"%s\", reason: %s\n", getName().c_str(), ex.what() );
    QFile::remove( QString::fromStdString( ftsIdxName ) );
  }
}

/// BglDictionary::findHeadwordsForSynonym()

class BglHeadwordsRequest: public Dictionary::WordSearchRequest
{
  wstring str;
  BglDictionary & dict;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  BglHeadwordsRequest( wstring const & word_, BglDictionary & dict_ ):
    str( word_ ),
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

  ~BglHeadwordsRequest() override
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void BglHeadwordsRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  vector< WordArticleLink > chain = dict.findArticles( str );

  wstring caseFolded = Folding::applySimpleCaseOnly( str );

  for ( auto & x : chain ) {
    if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
      finish();
      return;
    }

    string headword, displayedHeadword, articleText;

    dict.loadArticle( x.articleOffset, headword, displayedHeadword, articleText );

    wstring headwordDecoded;
    try {
      headwordDecoded = Utf8::decode( removePostfix( headword ) );
    }
    catch ( Utf8::exCantDecode & ) {
    }

    if ( caseFolded != Folding::applySimpleCaseOnly( headwordDecoded ) && !headwordDecoded.empty() ) {
      // The headword seems to differ from the input word, which makes the
      // input word its synonym.
      QMutexLocker _( &dataMutex );

      matches.push_back( headwordDecoded );
    }
  }

  finish();
}

sptr< Dictionary::WordSearchRequest > BglDictionary::findHeadwordsForSynonym( wstring const & word )

{
  return synonymSearchEnabled ? std::make_shared< BglHeadwordsRequest >( word, *this ) :
                                Class::findHeadwordsForSynonym( word );
}

// Converts a $1$-like postfix to a <sup>1</sup> one
string postfixToSuperscript( string const & in )
{
  if ( !in.size() || in[ in.size() - 1 ] != '$' )
    return in;

  for ( long x = in.size() - 2; x >= 0; x-- ) {
    if ( in[ x ] == '$' ) {
      if ( in.size() - x - 2 > 2 ) {
        // Large postfixes seem like something we wouldn't want to show --
        // some dictionaries seem to have each word numbered using the
        // postfix.
        return in.substr( 0, x );
      }
      else
        return in.substr( 0, x ) + "<sup>" + in.substr( x + 1, in.size() - x - 2 ) + "</sup>";
    }
    else if ( !isdigit( in[ x ] ) )
      break;
  }

  return in;
}


/// BglDictionary::getArticle()


class BglArticleRequest: public Dictionary::DataRequest
{
  wstring word;
  vector< wstring > alts;
  BglDictionary & dict;

  QAtomicInt isCancelled;
  bool ignoreDiacritics;
  QFuture< void > f;

public:

  BglArticleRequest( wstring const & word_,
                     vector< wstring > const & alts_,
                     BglDictionary & dict_,
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

  void fixHebString( string & hebStr );      // Hebrew support
  void fixHebArticle( string & hebArticle ); // Hebrew support

  ~BglArticleRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void BglArticleRequest::fixHebString( string & hebStr ) // Hebrew support - convert non-unicode to unicode
{
  wstring hebWStr;
  try {
    hebWStr = Utf8::decode( hebStr );
  }
  catch ( Utf8::exCantDecode & ) {
    hebStr = "Utf-8 decoding error";
    return;
  }

  for ( char32_t & i : hebWStr ) {
    if (
      ( i >= 224 && i <= 250 )
      || ( i >= 192
           && i
             <= 210 ) ) // Hebrew chars encoded ecoded as windows-1255 or ISO-8859-8, or as vowel-points of windows-1255
      i += 1488 - 224;  // Convert to Hebrew unicode
  }
  hebStr = Utf8::encode( hebWStr );
}

void BglArticleRequest::fixHebArticle( string & hebArticle ) // Hebrew support - remove extra chars at the end
{
  unsigned nulls;

  for ( nulls = hebArticle.size(); nulls > 0
        && ( ( hebArticle[ nulls - 1 ] <= 32 && hebArticle[ nulls - 1 ] >= 0 )
             || ( hebArticle[ nulls - 1 ] >= 65 && hebArticle[ nulls - 1 ] <= 90 ) );
        --nulls )
    ; //special chars and A-Z

  hebArticle.resize( nulls );
}

void BglArticleRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  vector< WordArticleLink > chain = dict.findArticles( word, ignoreDiacritics );

  static Language::Id hebrew = LangCoder::code2toInt( "he" ); // Hebrew support

  for ( const auto & alt : alts ) {
    /// Make an additional query for each alt

    vector< WordArticleLink > altChain = dict.findArticles( alt, ignoreDiacritics );

    chain.insert( chain.end(), altChain.begin(), altChain.end() );
  }

  multimap< wstring, pair< string, string > > mainArticles, alternateArticles;

  set< uint32_t > articlesIncluded; // Some synonims make it that the articles
                                    // appear several times. We combat this
                                    // by only allowing them to appear once.
  // Sometimes the articles are physically duplicated. We store hashes of
  // the bodies to account for this.
  set< QByteArray > articleBodiesIncluded;

  wstring wordCaseFolded = Folding::applySimpleCaseOnly( word );
  if ( ignoreDiacritics )
    wordCaseFolded = Folding::applyDiacriticsOnly( wordCaseFolded );

  for ( auto & x : chain ) {
    if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
      finish();
      return;
    }

    try {

      if ( articlesIncluded.find( x.articleOffset ) != articlesIncluded.end() )
        continue; // We already have this article in the body.

      // Now grab that article

      string headword, displayedHeadword, articleText;

      dict.loadArticle( x.articleOffset, headword, displayedHeadword, articleText );

      // Ok. Now, does it go to main articles, or to alternate ones? We list
      // main ones first, and alternates after.

      // We do the case-folded and postfix-less comparison here.

      wstring headwordStripped = Folding::applySimpleCaseOnly( removePostfix( headword ) );
      if ( ignoreDiacritics )
        headwordStripped = Folding::applyDiacriticsOnly( headwordStripped );

      // Hebrew support - fix Hebrew text
      if ( dict.idxHeader.langFrom == hebrew ) {
        displayedHeadword = displayedHeadword.size() ? displayedHeadword : headword;
        fixHebString( articleText );
        fixHebArticle( articleText );
        fixHebString( displayedHeadword );
      }

      string const & targetHeadword = displayedHeadword.size() ? displayedHeadword : headword;

      QCryptographicHash hash( QCryptographicHash::Md5 );
      hash.addData( targetHeadword.data(), targetHeadword.size() + 1 ); // with 0
      hash.addData( articleText.data(), articleText.size() );

      if ( !articleBodiesIncluded.insert( hash.result() ).second )
        continue; // Already had this body

      multimap< wstring, pair< string, string > > & mapToUse =
        ( wordCaseFolded == headwordStripped ) ? mainArticles : alternateArticles;

      mapToUse.insert( pair( Folding::applySimpleCaseOnly( headword ), pair( targetHeadword, articleText ) ) );

      articlesIncluded.insert( x.articleOffset );

    } // try
    catch ( std::exception & ex ) {
      gdWarning( "BGL: Failed loading article from \"%s\", reason: %s\n", dict.getName().c_str(), ex.what() );
    }
  }

  if ( mainArticles.empty() && alternateArticles.empty() ) {
    // No such word
    finish();
    return;
  }

  string result;

  multimap< wstring, pair< string, string > >::const_iterator i;

  string cleaner = Utils::Html::getHtmlCleaner();
  for ( i = mainArticles.begin(); i != mainArticles.end(); ++i ) {
    if ( dict.isFromLanguageRTL() ) // RTL support
      result += "<h3 style=\"text-align:right;direction:rtl\">";
    else
      result += "<h3>";
    result += postfixToSuperscript( i->second.first );
    result += "</h3>";
    if ( dict.isToLanguageRTL() )
      result += "<div class=\"bglrtl\">" + i->second.second + "</div>";
    else
      result += "<div>" + i->second.second + "</div>";
    result += cleaner;
  }


  for ( i = alternateArticles.begin(); i != alternateArticles.end(); ++i ) {
    if ( dict.isFromLanguageRTL() ) // RTL support
      result += "<h3 style=\"text-align:right;direction:rtl\">";
    else
      result += "<h3>";
    result += postfixToSuperscript( i->second.first );
    result += "</h3>";
    if ( dict.isToLanguageRTL() )
      result += "<div class=\"bglrtl\">" + i->second.second + "</div>";
    else
      result += "<div>" + i->second.second + "</div>";
    result += cleaner;
  }
  // Do some cleanups in the text

  BglDictionary::replaceCharsetEntities( result );

  result =
    QString::fromUtf8( result.c_str() )
      // onclick location to link
      .replace( QRegularExpression(
                  R"(<([a-z0-9]+)\s+[^>]*onclick="[a-z.]*location(?:\.href)\s*=\s*'([^']+)[^>]*>([^<]+)</\1>)",
                  QRegularExpression::CaseInsensitiveOption ),
                R"(<a href="\2">\3</a>)" )
      .replace(
        QRegularExpression( R"((<\s*a\s+[^>]*href\s*=\s*["']\s*)bword://)", QRegularExpression::CaseInsensitiveOption ),
        "\\1bword:" )
      //remove invalid width, height attrs
      .replace( QRegularExpression( R"((width|height)\s*=\s*["']\d{7,}["''])" ), "" )
      //remove invalid <br> tag
      .replace(
        QRegularExpression(
          R"(<br>(<div|<table|<tbody|<tr|<td|</div>|</table>|</tbody>|</tr>|</td>|function addScript|var scNode|scNode|var atag|while\(atag|atag=atag|document\.getElementsByTagName|addScript|src="bres|<a onmouseover="return overlib|onclick="return overlib))",
          QRegularExpression::CaseInsensitiveOption ),
        "\\1" )
      .replace(
        QRegularExpression(
          R"((AUTOSTATUS, WRAP\);" |</DIV>|addScript\('JS_FILE_PHONG_VT_45634'\);|appendChild\(scNode\);|atag\.firstChild;)<br>)",
          QRegularExpression::CaseInsensitiveOption ),
        " \\1 " )
      .toUtf8()
      .data();


  appendString( result );

  hasAnyData = true;

  finish();
}

sptr< Dictionary::DataRequest > BglDictionary::getArticle( wstring const & word,
                                                           vector< wstring > const & alts,
                                                           wstring const &,
                                                           bool ignoreDiacritics )

{
  return std::make_shared< BglArticleRequest >( word, alts, *this, ignoreDiacritics );
}


//// BglDictionary::getResource()

class BglResourceRequest: public Dictionary::DataRequest
{

  QMutex & idxMutex;
  File::Class & idx;
  uint32_t resourceListOffset, resourcesCount;
  string name;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  BglResourceRequest( QMutex & idxMutex_,
                      File::Class & idx_,
                      uint32_t resourceListOffset_,
                      uint32_t resourcesCount_,
                      string const & name_ ):
    idxMutex( idxMutex_ ),
    idx( idx_ ),
    resourceListOffset( resourceListOffset_ ),
    resourcesCount( resourcesCount_ ),
    name( name_ )
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

  ~BglResourceRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void BglResourceRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  string nameLowercased = name;

  for ( char & i : nameLowercased )
    i = tolower( i );

  QMutexLocker _( &idxMutex );

  idx.seek( resourceListOffset );

  for ( size_t count = resourcesCount; count--; ) {
    if ( Utils::AtomicInt::loadAcquire( isCancelled ) )
      break;

    vector< char > nameData( idx.read< uint32_t >() );
    idx.read( &nameData.front(), nameData.size() );

    for ( size_t x = nameData.size(); x--; )
      nameData[ x ] = tolower( nameData[ x ] );

    uint32_t offset = idx.read< uint32_t >();

    if ( string( &nameData.front(), nameData.size() ) == nameLowercased ) {
      // We have a match.

      idx.seek( offset );

      QMutexLocker _( &dataMutex );

      data.resize( idx.read< uint32_t >() );

      vector< unsigned char > compressedData( idx.read< uint32_t >() );

      idx.read( &compressedData.front(), compressedData.size() );

      unsigned long decompressedLength = data.size();

      if ( uncompress( (unsigned char *)&data.front(),
                       &decompressedLength,
                       &compressedData.front(),
                       compressedData.size() )
             != Z_OK
           || decompressedLength != data.size() ) {
        gdWarning( "Failed to decompress resource \"%s\", ignoring it.\n", name.c_str() );
      }
      else
        hasAnyData = true;

      break;
    }
  }

  finish();
}

sptr< Dictionary::DataRequest > BglDictionary::getResource( string const & name )

{
  return std::shared_ptr< BglResourceRequest >(
    new BglResourceRequest( idxMutex, idx, idxHeader.resourceListOffset, idxHeader.resourcesCount, name ) );
}

/// Replaces <CHARSET c="t">1234;</CHARSET> occurrences with &#x1234;
void BglDictionary::replaceCharsetEntities( string & text )
{
  QString str = QString::fromUtf8( text.c_str() );

  QRegularExpression charsetExp(
    R"(<\s*charset\s+c\s*=\s*["']?t["']?\s*>((?:\s*[0-9a-fA-F]+\s*;\s*)*)<\s*/\s*charset\s*>)",
    QRegularExpression::CaseInsensitiveOption | QRegularExpression::InvertedGreedinessOption );

  QRegularExpression oneValueExp( "\\s*([0-9a-fA-F]+)\\s*;" );
  QString result;
  int pos = 0;

  QRegularExpressionMatchIterator it = charsetExp.globalMatch( str );
  while ( it.hasNext() ) {
    QRegularExpressionMatch match = it.next();
    result += str.mid( pos, match.capturedStart() - pos );
    pos = match.capturedEnd();

    QRegularExpressionMatchIterator itValue = oneValueExp.globalMatch( match.captured( 1 ) );
    while ( itValue.hasNext() ) {
      QRegularExpressionMatch matchValue = itValue.next();
      result += "&#x" + matchValue.captured( 1 ) + ";";
    }
  }

  if ( pos ) {
    result += str.mid( pos );
    str = result;
  }


  text = str.toUtf8().data();
}

class ResourceHandler: public Babylon::ResourceHandler
{
  File::Class & idxFile;
  list< pair< string, uint32_t > > resources;

public:

  ResourceHandler( File::Class & idxFile_ ):
    idxFile( idxFile_ )
  {
  }

  list< pair< string, uint32_t > > const & getResources() const
  {
    return resources;
  }

protected:
  void handleBabylonResource( string const & filename, char const * data, size_t size ) override;
};

void ResourceHandler::handleBabylonResource( string const & filename, char const * data, size_t size )
{
  //GD_DPRINTF( "Handling resource file %s (%u bytes)\n", filename.c_str(), size );

  vector< unsigned char > compressedData( compressBound( size ) );

  unsigned long compressedSize = compressedData.size();

  if ( compress( &compressedData.front(), &compressedSize, (unsigned char const *)data, size ) != Z_OK ) {
    gdWarning( "Failed to compress the body of resource \"%s\", dropping it.\n", filename.c_str() );
    return;
  }

  resources.push_back( pair< string, uint32_t >( filename, idxFile.tell() ) );

  idxFile.write< uint32_t >( size );
  idxFile.write< uint32_t >( compressedSize );
  idxFile.write( &compressedData.front(), compressedSize );
}
} // namespace

sptr< Dictionary::DataRequest > BglDictionary::getSearchResults( QString const & searchString,
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


vector< sptr< Dictionary::Class > > makeDictionaries( vector< string > const & fileNames,
                                                      string const & indicesDir,
                                                      Dictionary::Initializing & initializing )

{
  vector< sptr< Dictionary::Class > > dictionaries;

  for ( const auto & fileName : fileNames ) {
    // Skip files with the extensions different to .bgl to speed up the
    // scanning
    if ( !Utils::endsWithIgnoreCase( fileName, ".bgl" ) )
      continue;

    // Got the file -- check if we need to rebuid the index

    vector< string > dictFiles( 1, fileName );

    string dictId = Dictionary::makeDictionaryId( dictFiles );

    string indexFile = indicesDir + dictId;

    if ( Dictionary::needToRebuildIndex( dictFiles, indexFile ) || indexIsOldOrBad( indexFile ) ) {
      // Building the index

      gdDebug( "Bgl: Building the index for dictionary: %s\n", fileName.c_str() );

      try {
        Babylon b( fileName );

        if ( !b.open() )
          continue;

        std::string sourceCharset, targetCharset;

        if ( !b.read( sourceCharset, targetCharset ) ) {
          gdWarning( "Failed to start reading from %s, skipping it\n", fileName.c_str() );
          continue;
        }

        initializing.indexingDictionary( b.title() );

        File::Class idx( indexFile, "wb" );

        IdxHeader idxHeader;

        memset( &idxHeader, 0, sizeof( idxHeader ) );

        // We write a dummy header first. At the end of the process the header
        // will be rewritten with the right values.

        idx.write( idxHeader );

        idx.write< uint32_t >( b.title().size() );
        idx.write( b.title().data(), b.title().size() );

        // This is our index data that we accumulate during the loading process.
        // For each new word encountered, we emit the article's body to the file
        // immediately, inserting the word itself and its offset in this map.
        // This map maps folded words to the original words and the corresponding
        // articles' offsets.
        IndexedWords indexedWords;

        // We use this buffer to decode utf8 into it.
        vector< wchar > wcharBuffer;

        ChunkedStorage::Writer chunks( idx );

        uint32_t articleCount = 0, wordCount = 0;

        ResourceHandler resourceHandler( idx );

        b.setResourcePrefix( string( "bres://" ) + dictId + "/" );

        // Save icon if there's one
        if ( size_t sz = b.getIcon().size() ) {
          idxHeader.iconAddress = chunks.startNewBlock();
          chunks.addToBlock( &b.getIcon().front(), sz );
          idxHeader.iconSize = sz;
        }

        // Save dictionary description if there's one
        idxHeader.descriptionSize    = 0;
        idxHeader.descriptionAddress = chunks.startNewBlock();

        chunks.addToBlock( b.copyright().c_str(), b.copyright().size() + 1 );
        idxHeader.descriptionSize += b.copyright().size() + 1;

        chunks.addToBlock( b.author().c_str(), b.author().size() + 1 );
        idxHeader.descriptionSize += b.author().size() + 1;

        chunks.addToBlock( b.email().c_str(), b.email().size() + 1 );
        idxHeader.descriptionSize += b.email().size() + 1;

        chunks.addToBlock( b.description().c_str(), b.description().size() + 1 );
        idxHeader.descriptionSize += b.description().size() + 1;

        for ( ;; ) {
          bgl_entry e = b.readEntry( &resourceHandler );

          if ( e.headword.empty() )
            break;

          // Save the article's body itself first

          uint32_t articleAddress = chunks.startNewBlock();

          chunks.addToBlock( e.headword.c_str(), e.headword.size() + 1 );
          chunks.addToBlock( e.displayedHeadword.c_str(), e.displayedHeadword.size() + 1 );
          chunks.addToBlock( e.definition.c_str(), e.definition.size() + 1 );

          // Add entries to the index

          addEntryToIndex( e.headword, articleAddress, indexedWords, wcharBuffer );

          for ( auto & alternate : e.alternates )
            addEntryToIndex( alternate, articleAddress, indexedWords, wcharBuffer );

          wordCount += 1 + e.alternates.size();
          ++articleCount;
        }

        // Finish with the chunks

        idxHeader.chunksOffset = chunks.finish();

        GD_DPRINTF( "Writing index...\n" );

        // Good. Now build the index

        IndexInfo idxInfo = BtreeIndexing::buildIndex( indexedWords, idx );

        idxHeader.indexBtreeMaxElements = idxInfo.btreeMaxElements;
        idxHeader.indexRootOffset       = idxInfo.rootOffset;

        // Save the resource's list.

        idxHeader.resourceListOffset = idx.tell();
        idxHeader.resourcesCount     = resourceHandler.getResources().size();

        for ( const auto & j : resourceHandler.getResources() ) {
          idx.write< uint32_t >( j.first.size() );
          idx.write( j.first.data(), j.first.size() );
          idx.write< uint32_t >( j.second );
        }

        // That concludes it. Update the header.

        idxHeader.signature      = Signature;
        idxHeader.formatVersion  = CurrentFormatVersion;
        idxHeader.parserVersion  = Babylon::ParserVersion;
        idxHeader.foldingVersion = Folding::Version;
        idxHeader.articleCount   = articleCount;
        idxHeader.wordCount      = wordCount;
        idxHeader.langFrom       = b.sourceLang(); //LangCoder::findIdForLanguage( Utf8::decode( b.sourceLang() ) );
        idxHeader.langTo         = b.targetLang(); //LangCoder::findIdForLanguage( Utf8::decode( b.targetLang() ) );

        idx.rewind();

        idx.write( &idxHeader, sizeof( idxHeader ) );
      }
      catch ( std::exception & e ) {
        gdWarning( "BGL dictionary indexing failed: %s, error: %s\n", fileName.c_str(), e.what() );
      }
    }

    try {
      dictionaries.push_back( std::make_shared< BglDictionary >( dictId, indexFile, fileName ) );
    }
    catch ( std::exception & e ) {
      gdWarning( "BGL dictionary initializing failed: %s, error: %s\n", fileName.c_str(), e.what() );
    }
  }

  return dictionaries;
}

} // namespace Bgl
