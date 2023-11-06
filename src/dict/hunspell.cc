/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "hunspell.hh"
#include "utf8.hh"
#include "htmlescape.hh"
#include "iconv.hh"
#include "folding.hh"
#include "wstring_qt.hh"
#include "language.hh"
#include "langcoder.hh"

#include <QRunnable>
#include <QThreadPool>
#include <QSemaphore>
#if ( QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 ) )
  #include <QtCore5Compat/QRegExp>
#else
  #include <QRegExp>
#endif
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>

#include <set>
#ifndef INCLUDE_LIBRARY_PATH
  #include <hunspell.hxx>
#else
  #include <hunspell/hunspell.hxx>
#endif
#include "gddebug.hh"

#include "utils.hh"
#include <QtConcurrent>

namespace HunspellMorpho {

using namespace Dictionary;

using gd::wchar;

namespace {

class HunspellDictionary: public Dictionary::Class
{
  string name;
  Hunspell hunspell;

#ifdef Q_OS_WIN32
  static string Utf8ToLocal8Bit( string const & name )
  {
    return string( QString::fromUtf8( name.c_str() ).toLocal8Bit().data() );
  }
#endif

public:

  /// files[ 0 ] should be .aff file, files[ 1 ] should be .dic file.
  HunspellDictionary( string const & id, string const & name_, vector< string > const & files ):
    Dictionary::Class( id, files ),
    name( name_ ),
#ifdef Q_OS_WIN32
    hunspell( Utf8ToLocal8Bit( files[ 0 ] ).c_str(), Utf8ToLocal8Bit( files[ 1 ] ).c_str() )
#else
    hunspell( files[ 0 ].c_str(), files[ 1 ].c_str() )
#endif
  {
  }

  string getName() noexcept override
  {
    return name;
  }

  map< Property, string > getProperties() noexcept override
  {
    return map< Property, string >();
  }

  unsigned long getArticleCount() noexcept override
  {
    return 0;
  }

  unsigned long getWordCount() noexcept override
  {
    return 0;
  }

  sptr< WordSearchRequest > prefixMatch( wstring const &, unsigned long maxResults ) override;

  sptr< WordSearchRequest > findHeadwordsForSynonym( wstring const & ) override;

  sptr< DataRequest > getArticle( wstring const &, vector< wstring > const & alts, wstring const &, bool ) override;

  bool isLocalDictionary() override
  {
    return true;
  }

  vector< wstring > getAlternateWritings( const wstring & word ) noexcept override;

protected:

  void loadIcon() noexcept override;

private:

  // We used to have a separate mutex for each Hunspell instance, assuming
  // that its code was reentrant (though probably not thread-safe). However,
  // crashes were discovered later when using several Hunspell dictionaries
  // simultaneously, and we've switched to have a single mutex for all hunspell
  // calls - evidently it's not really reentrant.
  static QMutex & getHunspellMutex()
  {
    static QMutex mutex;
    return mutex;
  }
  //  QMutex hunspellMutex;
};

/// Encodes the given string to be passed to the hunspell object. May throw
/// Iconv::Ex
string encodeToHunspell( Hunspell &, wstring const & );

/// Decodes the given string returned by the hunspell object. May throw
/// Iconv::Ex
wstring decodeFromHunspell( Hunspell &, char const * );

/// Generates suggestions via hunspell
QVector< wstring > suggest( wstring & word, QMutex & hunspellMutex, Hunspell & hunspell );

/// Generates suggestions for compound expression
void getSuggestionsForExpression( wstring const & expression,
                                  vector< wstring > & suggestions,
                                  QMutex & hunspellMutex,
                                  Hunspell & hunspell );

/// Returns true if the string contains whitespace, false otherwise
bool containsWhitespace( wstring const & str )
{
  wchar const * next = str.c_str();

  for ( ; *next; ++next )
    if ( Folding::isWhitespace( *next ) )
      return true;

  return false;
}

void HunspellDictionary::loadIcon() noexcept
{
  if ( dictionaryIconLoaded )
    return;

  QString fileName = QDir::fromNativeSeparators( getDictionaryFilenames()[ 0 ].c_str() );

  // Remove the extension
  fileName.chop( 3 );

  if ( !loadIconFromFile( fileName ) ) {
    // Load failed -- use default icons
    dictionaryIcon = QIcon( ":/icons/icon32_hunspell.png" );
  }

  dictionaryIconLoaded = true;
}

vector< wstring > HunspellDictionary::getAlternateWritings( wstring const & word ) noexcept
{
  vector< wstring > results;

  if ( containsWhitespace( word ) ) {
    getSuggestionsForExpression( word, results, getHunspellMutex(), hunspell );
  }

  return results;
}

/// HunspellDictionary::getArticle()

class HunspellArticleRequest: public Dictionary::DataRequest
{

  QMutex & hunspellMutex;
  Hunspell & hunspell;
  wstring word;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  HunspellArticleRequest( wstring const & word_, QMutex & hunspellMutex_, Hunspell & hunspell_ ):
    hunspellMutex( hunspellMutex_ ),
    hunspell( hunspell_ ),
    word( word_ )
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

  ~HunspellArticleRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};

void HunspellArticleRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  vector< string > suggestions;

  try {
    wstring trimmedWord = Folding::trimWhitespaceOrPunct( word );

    if ( containsWhitespace( trimmedWord ) ) {
      // For now we don't analyze whitespace-containing phrases
      finish();
      return;
    }

    QMutexLocker _( &hunspellMutex );

    string encodedWord = encodeToHunspell( hunspell, trimmedWord );

    if ( hunspell.spell( encodedWord ) ) {
      // Good word -- no spelling suggestions then.
      finish();
      return;
    }

    suggestions = hunspell.suggest( encodedWord );
    if ( !suggestions.empty() ) {
      // There were some suggestions made for us. Make an appropriate output.

      string result = "<div class=\"gdspellsuggestion\">"
        + Html::escape( QCoreApplication::translate( "Hunspell", "Spelling suggestions: " ).toUtf8().data() );

      wstring lowercasedWord = Folding::applySimpleCaseOnly( word );

      for ( vector< string >::size_type x = 0; x < suggestions.size(); ++x ) {
        wstring suggestion = decodeFromHunspell( hunspell, suggestions[ x ].c_str() );

        if ( Folding::applySimpleCaseOnly( suggestion ) == lowercasedWord ) {
          // If among suggestions we see the same word just with the different
          // case, we botch the search -- our searches are case-insensitive, and
          // there's no need for suggestions on a good word.

          finish();

          return;
        }
        string suggestionUtf8 = Utf8::encode( suggestion );

        result += "<a href=\"bword:";
        result += Html::escape( suggestionUtf8 ) + "\">";
        result += Html::escape( suggestionUtf8 ) + "</a>";

        if ( x != suggestions.size() - 1 )
          result += ", ";
      }

      result += "</div>";

      appendString( result );

      hasAnyData = true;
    }
  }
  catch ( Iconv::Ex & e ) {
    gdWarning( "Hunspell: charset conversion error, no processing's done: %s\n", e.what() );
  }
  catch ( std::exception & e ) {
    gdWarning( "Hunspell: error: %s\n", e.what() );
  }

  finish();
}

sptr< DataRequest >
HunspellDictionary::getArticle( wstring const & word, vector< wstring > const &, wstring const &, bool )

{
  return std::make_shared< HunspellArticleRequest >( word, getHunspellMutex(), hunspell );
}

/// HunspellDictionary::findHeadwordsForSynonym()

class HunspellHeadwordsRequest: public Dictionary::WordSearchRequest
{

  QMutex & hunspellMutex;
  Hunspell & hunspell;
  wstring word;

  QAtomicInt isCancelled;
  QFuture< void > f;


public:

  HunspellHeadwordsRequest( wstring const & word_, QMutex & hunspellMutex_, Hunspell & hunspell_ ):
    hunspellMutex( hunspellMutex_ ),
    hunspell( hunspell_ ),
    word( word_ )
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

  ~HunspellHeadwordsRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};


void HunspellHeadwordsRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  wstring trimmedWord = Folding::trimWhitespaceOrPunct( word );

  if ( trimmedWord.size() > 80 ) {
    // We won't do anything for overly long sentences since that would probably
    // only waste time.
    finish();
    return;
  }

  if ( containsWhitespace( trimmedWord ) ) {
    vector< wstring > results;

    getSuggestionsForExpression( trimmedWord, results, hunspellMutex, hunspell );

    QMutexLocker _( &dataMutex );
    for ( const auto & result : results )
      matches.push_back( result );
  }
  else {
    QVector< wstring > suggestions = suggest( trimmedWord, hunspellMutex, hunspell );

    if ( !suggestions.empty() ) {
      QMutexLocker _( &dataMutex );

      for ( const auto & suggestion : suggestions )
        matches.push_back( suggestion );
    }
  }

  finish();
}

QVector< wstring > suggest( wstring & word, QMutex & hunspellMutex, Hunspell & hunspell )
{
  QVector< wstring > result;

  vector< string > suggestions;

  try {
    QMutexLocker _( &hunspellMutex );

    string encodedWord = encodeToHunspell( hunspell, word );

    suggestions = hunspell.analyze( encodedWord );
    if ( !suggestions.empty() ) {
      // There were some suggestions made for us. Make an appropriate output.

      wstring lowercasedWord = Folding::applySimpleCaseOnly( word );

      static QRegExp cutStem( R"(^\s*st:(((\s+(?!\w{2}:)(?!-)(?!\+))|\S+)+))" );

      for ( const auto & x : suggestions ) {
        QString suggestion = QString::fromStdU32String( decodeFromHunspell( hunspell, x.c_str() ) );

        // Strip comments
        int n = suggestion.indexOf( '#' );
        if ( n >= 0 )
          suggestion.chop( suggestion.length() - n );

        GD_DPRINTF( ">>>Sugg: %s\n", suggestion.toLocal8Bit().data() );

        if ( cutStem.indexIn( suggestion.trimmed() ) != -1 ) {
          wstring alt = gd::toWString( cutStem.cap( 1 ) );

          if ( Folding::applySimpleCaseOnly( alt ) != lowercasedWord ) // No point in providing same word
          {
#ifdef QT_DEBUG
            qDebug() << ">>>>>Alt:" << QString::fromStdU32String( alt );
#endif
            result.append( alt );
          }
        }
      }
    }
  }
  catch ( Iconv::Ex & e ) {
    gdWarning( "Hunspell: charset conversion error, no processing's done: %s\n", e.what() );
  }

  return result;
}


sptr< WordSearchRequest > HunspellDictionary::findHeadwordsForSynonym( wstring const & word )

{
  return std::make_shared< HunspellHeadwordsRequest >( word, getHunspellMutex(), hunspell );
}


/// HunspellDictionary::prefixMatch()

class HunspellPrefixMatchRequest: public Dictionary::WordSearchRequest
{

  QMutex & hunspellMutex;
  Hunspell & hunspell;
  wstring word;

  QAtomicInt isCancelled;
  QFuture< void > f;

public:

  HunspellPrefixMatchRequest( wstring const & word_, QMutex & hunspellMutex_, Hunspell & hunspell_ ):
    hunspellMutex( hunspellMutex_ ),
    hunspell( hunspell_ ),
    word( word_ )
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

  ~HunspellPrefixMatchRequest()
  {
    isCancelled.ref();
    f.waitForFinished();
  }
};


void HunspellPrefixMatchRequest::run()
{
  if ( Utils::AtomicInt::loadAcquire( isCancelled ) ) {
    finish();
    return;
  }

  try {
    wstring trimmedWord = Folding::trimWhitespaceOrPunct( word );

    if ( trimmedWord.empty() || containsWhitespace( trimmedWord ) ) {
      // For now we don't analyze whitespace-containing phrases
      finish();
      return;
    }

    QMutexLocker _( &hunspellMutex );

    string encodedWord = encodeToHunspell( hunspell, trimmedWord );

    if ( hunspell.spell( encodedWord ) ) {
      // Known word -- add it to the result

      QMutexLocker _( &dataMutex );

      matches.push_back( WordMatch( trimmedWord, 1 ) );
    }
  }
  catch ( Iconv::Ex & e ) {
    gdWarning( "Hunspell: charset conversion error, no processing's done: %s\n", e.what() );
  }

  finish();
}

sptr< WordSearchRequest > HunspellDictionary::prefixMatch( wstring const & word, unsigned long /*maxResults*/ )

{
  return std::make_shared< HunspellPrefixMatchRequest >( word, getHunspellMutex(), hunspell );
}

void getSuggestionsForExpression( wstring const & expression,
                                  vector< wstring > & suggestions,
                                  QMutex & hunspellMutex,
                                  Hunspell & hunspell )
{
  // Analyze each word separately and use the first two suggestions, if any.
  // This is useful for compound expressions where some words is
  // in different form, e.g. "dozing off" -> "doze off".

  wstring trimmedWord = Folding::trimWhitespaceOrPunct( expression );
  wstring word, punct;
  QVector< wstring > words;

  suggestions.clear();

  // Parse string to separate words

  for ( wchar const * c = trimmedWord.c_str();; ++c ) {
    if ( !*c || Folding::isPunct( *c ) || Folding::isWhitespace( *c ) ) {
      if ( word.size() ) {
        words.push_back( word );
        word.clear();
      }
      if ( *c )
        punct.push_back( *c );
    }
    else {
      if ( punct.size() ) {
        words.push_back( punct );
        punct.clear();
      }
      if ( *c )
        word.push_back( *c );
    }
    if ( !*c )
      break;
  }

  if ( words.size() > 21 ) {
    // Too many words - no suggestions
    return;
  }

  // Combine result strings from suggestions

  QVector< wstring > results;

  for ( const auto & i : words ) {
    word = i;
    if ( Folding::isPunct( word[ 0 ] ) || Folding::isWhitespace( word[ 0 ] ) ) {
      for ( auto & result : results )
        result.append( word );
    }
    else {
      QVector< wstring > sugg = suggest( word, hunspellMutex, hunspell );
      int suggNum             = sugg.size() + 1;
      if ( suggNum > 3 )
        suggNum = 3;
      int resNum = results.size();
      wstring resultStr;

      if ( resNum == 0 ) {
        for ( int k = 0; k < suggNum; k++ )
          results.push_back( k == 0 ? word : sugg.at( k - 1 ) );
      }
      else {
        for ( int j = 0; j < resNum; j++ ) {
          resultStr = results.at( j );
          for ( int k = 0; k < suggNum; k++ ) {
            if ( k == 0 )
              results[ j ].append( word );
            else
              results.push_back( resultStr + sugg.at( k - 1 ) );
          }
        }
      }
    }
  }

  for ( const auto & result : results )
    if ( result != trimmedWord )
      suggestions.push_back( result );
}

string encodeToHunspell( Hunspell & hunspell, wstring const & str )
{
  Iconv conv( Iconv::GdWchar );

  void const * in = str.data();
  size_t inLeft   = str.size() * sizeof( wchar );

  vector< char > result( str.size() * 4 + 1 ); // +1 isn't actually needed,
                                               // but then iconv complains on empty
                                               // words

  void * out     = &result.front();
  size_t outLeft = result.size();

  QString convStr = conv.convert( in, inLeft );
  return convStr.toStdString();
}

wstring decodeFromHunspell( Hunspell & hunspell, char const * str )
{
  Iconv conv( hunspell.get_dic_encoding() );

  void const * in = str;
  size_t inLeft   = strlen( str );

  vector< wchar > result( inLeft + 1 ); // +1 isn't needed, but see above

  void * out     = &result.front();
  size_t outLeft = result.size() * sizeof( wchar );

  QString convStr = conv.convert( in, inLeft );
  return gd::toWString( convStr );
}
} // namespace

vector< sptr< Dictionary::Class > > makeDictionaries( Config::Hunspell const & cfg )

{
  vector< sptr< Dictionary::Class > > result;

  vector< DataFiles > dataFiles = findDataFiles( cfg.dictionariesPath );


  for ( const auto & enabledDictionarie : cfg.enabledDictionaries ) {
    for ( unsigned d = dataFiles.size(); d--; ) {
      if ( dataFiles[ d ].dictId == enabledDictionarie ) {
        // Found it

        vector< string > dictFiles;

        dictFiles.push_back( QDir::toNativeSeparators( dataFiles[ d ].affFileName ).toStdString() );
        dictFiles.push_back( QDir::toNativeSeparators( dataFiles[ d ].dicFileName ).toStdString() );

        result.push_back( std::make_shared< HunspellDictionary >( Dictionary::makeDictionaryId( dictFiles ),
                                                                  dataFiles[ d ].dictName.toUtf8().data(),
                                                                  dictFiles ) );
        break;
      }
    }
  }

  return result;
}

vector< DataFiles > findDataFiles( QString const & path )
{
  // Empty path means unconfigured directory
  if ( path.isEmpty() )
    return vector< DataFiles >();

  QDir dir( path );

  // Find all affix files

  QFileInfoList affixFiles = dir.entryInfoList( ( QStringList() << "*.aff"
                                                                << "*.AFF" ),
                                                QDir::Files );

  vector< DataFiles > result;
  std::set< QString > presentNames;

  for ( QFileInfoList::const_iterator i = affixFiles.constBegin(); i != affixFiles.constEnd(); ++i ) {
    QString affFileName = i->absoluteFilePath();

    // See if there's a corresponding .dic file
    QString dicFileNameBase = affFileName.mid( 0, affFileName.size() - 3 );

    QString dicFileName = dicFileNameBase + "dic";

    if ( !QFile( dicFileName ).exists() ) {
      dicFileName = dicFileNameBase + "DIC";
      if ( !QFile( dicFileName ).exists() )
        continue; // No dic file
    }

    QString dictId = i->fileName();
    dictId.chop( 4 );

    QString dictBaseId =
      dictId.size() < 3 ? dictId : ( ( dictId[ 2 ] == '-' || dictId[ 2 ] == '_' ) ? dictId.mid( 0, 2 ) : QString() );

    dictBaseId = dictBaseId.toLower();

    // Try making up good readable name from dictBaseId

    QString localizedName;

    if ( dictBaseId.size() == 2 )
      localizedName = Language::localizedNameForId( LangCoder::code2toInt( dictBaseId.toLatin1().data() ) );

    QString dictName = dictId;

    if ( localizedName.size() ) {
      dictName = localizedName;

      if ( dictId.size() > 2 && ( dictId[ 2 ] == '-' || dictId[ 2 ] == '_' )
           && dictId.mid( 3 ).toLower() != dictBaseId )
        dictName += " (" + dictId.mid( 3 ) + ")";
    }

    dictName = QCoreApplication::translate( "Hunspell", "%1 Morphology" ).arg( dictName );

    if ( presentNames.insert( dictName ).second ) {
      // Only include dictionaries with unique names. This combats stuff
      // like symlinks en-US->en_US and such

      result.push_back( DataFiles( affFileName, dicFileName, dictId, dictName ) );
    }
  }

  return result;
}

} // namespace HunspellMorpho
