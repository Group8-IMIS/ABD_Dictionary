#ifndef __DECOMPRESS_HH_INCLUDED__
#define __DECOMPRESS_HH_INCLUDED__

#include <QByteArray>
#include <string>

using std::string;

QByteArray zlibDecompress( const char * bufptr, unsigned length );

string decompressZlib( const char * bufptr, unsigned length );

string decompressBzip2( const char * bufptr, unsigned length );

string decompressLzma2( const char * bufptr, unsigned length, bool raw_decoder = false );

#endif // DECOMPRESS_HH
