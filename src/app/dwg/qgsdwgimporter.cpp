/***************************************************************************
                         qgsdwgimporter.cpp
                         --------------
    begin                : May 2016
    copyright            : (C) 2016 by Juergen E. Fischer
    email                : jef at norbit dot de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsdwgimporter.h"

#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "libdwgr.h"
#include "libdxfrw.h"
#include "qgsslconnect.h"
#include "qgsproviderregistry.h"
#include "qgis.h"

#include <QString>
#include <QFileInfo>

#include <sqlite3.h>

#define LOG( x ) QgsMessageLog::logMessage( x, QObject::tr( "DWG/DXF import" ) )
#define ONCE( x ) { static bool show=true; if( show ) LOG( x ); show=false; }
#define NYI( x ) { static bool show=true; if( show ) LOG( QObject::tr("Not yet implemented %1").arg( x ) ); show=false; }

static QString quotedIdentifier( QString id )
{
  id.replace( '\"', "\"\"" );
  return id.prepend( '\"' ).append( '\"' );
}

QString quotedValue( QString value )
{
  if ( value.isNull() )
    return "NULL";

  value.replace( '\'', "''" );
  return value.prepend( '\'' ).append( '\'' );
}

QgsDwgImporter::QgsDwgImporter( const QString &database )
    : mDb( nullptr )
    , mDatabase( database )
    , mDrawing( -1 )
{
  QgsDebugCall;

  QFile::remove( mDatabase );  // TODO remove me

  if ( !QFileInfo( mDatabase ).exists() )
  {
    // create database

    mDb = nullptr;

    QString spatialite_lib = QgsProviderRegistry::instance()->library( "spatialite" );
    QSharedPointer<QLibrary> myLib( new QLibrary( spatialite_lib ) );
    if ( !myLib->load() )
    {
      LOG( QObject::tr( "Could not load spatialite provider" ) );
      return;
    }

    typedef bool ( *createDbProc )( const QString&, QString& );
    createDbProc createDbPtr = ( createDbProc ) cast_to_fptr( myLib->resolve( "createDb" ) );
    if ( !createDbPtr )
    {
      LOG( QObject::tr( "Database creation function not found in spatialite provider" ) );
      return;
    }

    QString errCause;
    bool created = createDbPtr( mDatabase, errCause );
    if ( !created )
    {
      LOG( QObject::tr( "Database creation %1 failed [%2]" ).arg( mDatabase ).arg( errCause ) );
      return;
    }

    if ( QgsSLConnect::sqlite3_open( mDatabase.toUtf8(), &mDb ) != SQLITE_OK )
    {
      LOG( QObject::tr( "Could not open created database %1 failed" ).arg( mDatabase ) );
      mDb = nullptr;
      return;
    }

    startTransaction();

    QStringList sqls;
    sqls << "CREATE TABLE drawings(d INTEGER PRIMARY KEY, path TEXT, comments TEXT, importedat TEXT, lastmodified TEXT, UNIQUE(path))";
    sqls << "CREATE TABLE headers(d INTEGER, k TEXT, v TEXT, PRIMARY KEY(d,k), FOREIGN KEY (d) REFERENCES drawings(d) ON DELETE CASCADE)";
    sqls << "CREATE TABLE linetypes(d INTEGER, name TEXT, desc TEXT, path TEXT, PRIMARY KEY(d,name), FOREIGN KEY (d) REFERENCES drawings(d) ON DELETE CASCADE)";
    sqls << "CREATE TABLE layers(d INTEGER, name TEXT, linetype TEXT, color INTEGER, color24 INTEGER, lweight INTEGER, PRIMARY KEY(d,name), FOREIGN KEY (d) REFERENCES drawings(d) ON DELETE CASCADE, FOREIGN KEY (d,linetype) REFERENCES linetypes(d,name), FOREIGN KEY (d) REFERENCES drawings(d) ON DELETE CASCADE)";
    sqls << "CREATE TABLE dimstyles(d INTEGER, name TEXT,dimpost TEXT,dimapost TEXT,dimblk TEXT,dimblk1 TEXT,dimblk2 TEXT,dimscale DOUBLE PRECISION, dimasz DOUBLE PRECISION, dimexo DOUBLE PRECISION, dimdli DOUBLE PRECISION, dimexe DOUBLE PRECISION, dimrnd DOUBLE PRECISION, dimdle DOUBLE PRECISION, dimtp DOUBLE PRECISION, dimtm DOUBLE PRECISION, dimfxl DOUBLE PRECISION, dimtxt DOUBLE PRECISION, dimcen DOUBLE PRECISION, dimtsz DOUBLE PRECISION, dimaltf DOUBLE PRECISION, dimlfac DOUBLE PRECISION, dimtvp DOUBLE PRECISION, dimtfac DOUBLE PRECISION, dimgap DOUBLE PRECISION, dimaltrnd DOUBLE PRECISION, dimtol INTEGER, dimlim INTEGER, dimtih INTEGER, dimtoh INTEGER, dimse1 INTEGER, dimse2 INTEGER, dimtad INTEGER, dimzin INTEGER, dimazin INTEGER, dimalt INTEGER, dimaltd INTEGER, dimtofl INTEGER, dimsah INTEGER, dimtix INTEGER, dimsoxd INTEGER, dimclrd INTEGER, dimclre INTEGER, dimclrt INTEGER, dimadec INTEGER, dimunit INTEGER, dimdec INTEGER, dimtdec INTEGER, dimaltu INTEGER, dimalttd INTEGER, dimaunit INTEGER, dimfrac INTEGER, dimlunit INTEGER, dimdsep INTEGER, dimtmove INTEGER, dimjust INTEGER, dimsd1 INTEGER, dimsd2 INTEGER, dimtolj INTEGER, dimtzin INTEGER, dimaltz INTEGER, dimaltttz INTEGER, dimfit INTEGER, dimupt INTEGER, dimatfit INTEGER, dimfxlon INTEGER, dimtxsty TEXT, dimldrblk TEXT, dimlwd INTEGER, dimlwe INTEGER, PRIMARY KEY(d,name), FOREIGN KEY (d) REFERENCES drawings(d) ON DELETE CASCADE)";
    sqls << "CREATE TABLE textstyles(d INTEGER, name TEXT,height DOUBLE PRECISION,width DOUBLE PRECISION,oblique DOUBLE PRECISION,genFlag INTEGER,lastHeight DOUBLE PRECISION,font TEXT,bigFont TEXT,fontFamily INTEGER,PRIMARY KEY(d,name))";
    sqls << "CREATE TABLE entities(d INTEGER, handle INTEGER,etype INTEGER,space INTEGER,layer TEXT,lineType TEXT,color INTEGER,color24 INTEGER,transparency INTEGER,lweight INTEGER,ltscale DOUBLE PRECISION,visible INTEGER, FOREIGN KEY(d) REFERENCES drawings(d) ON DELETE CASCADE)";
    sqls << "CREATE TABLE appdata(d INTEGER, handle INTEGER, i INTEGER, value TEXT, PRIMARY KEY(d,handle,i), FOREIGN KEY(d,handle) REFERENCES entities(d,handle) ON DELETE CASCADE)";

    sqls << "CREATE TABLE points(d INTEGER, handle INTEGER, PRIMARY KEY(d,handle), FOREIGN KEY(d,handle) REFERENCES entities(d,handle) ON DELETE CASCADE)";
    sqls << "SELECT AddGeometryColumn('points','geom',0,'POINTZ',3)";
    sqls << "SELECT AddGeometryColumn('points','extpoint',0,'POINTZ',3)";

    sqls << "CREATE TABLE lines(d INTEGER, handle INTEGER, PRIMARY KEY(d,handle), FOREIGN KEY(d,handle) REFERENCES entities(d,handle) ON DELETE CASCADE)";
    sqls << "SELECT AddGeometryColumn('lines','geom',0,'POINTZ',3)";
    sqls << "SELECT AddGeometryColumn('lines','extpoint',0,'POINTZ',3)";

    Q_FOREACH ( const QString &sql, sqls )
    {
      if ( !exec( sql ) )
      {
        sqlite3_close( mDb );
        mDb = nullptr;
        return;
      }
    }

    commitTransaction();
  }
}

bool QgsDwgImporter::exec( QString sql )
{
  bool success = true;
  char *errmsg = nullptr;
  int rc = sqlite3_exec( mDb, sql.toUtf8(), nullptr, nullptr, &errmsg );
  if ( rc != SQLITE_OK )
  {
    LOG( QObject::tr( "SQL statement failed\nDatabase:%1\nSQL:%2\nError:%3" )
         .arg( mDatabase )
         .arg( sql )
         .arg( errmsg ? errmsg : QObject::tr( "unknown error" ) ) );
    success = false;
  }

  if ( errmsg )
    sqlite3_free( errmsg );

  return success;
}


void QgsDwgImporter::startTransaction()
{
  Q_ASSERT( mDb );

  ( void )sqlite3_exec( mDb, "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr );
  ( void )sqlite3_exec( mDb, "PRAGMA synchronous=OFF", nullptr, nullptr, nullptr );
  ( void )sqlite3_exec( mDb, "PRAGMA journal_mode=MEMORY", nullptr, nullptr, nullptr );

  ( void )sqlite3_exec( mDb, "BEGIN", nullptr, nullptr, nullptr );
}

void QgsDwgImporter::commitTransaction()
{
  Q_ASSERT( mDb );

  ( void )sqlite3_exec( mDb, "COMMIT", nullptr, nullptr, nullptr );
}

QgsDwgImporter::~QgsDwgImporter()
{
  QgsDebugCall;

  commitTransaction();

  QgsSLConnect::sqlite3_close( mDb );
}

bool QgsDwgImporter::import( const QString &drawing )
{
  QgsDebugCall;
  if ( !mDb )
  {
    QgsDebugMsg( "no open database" );
    return false;
  }

  QFileInfo fi( drawing );
  if ( !fi.isReadable() )
  {
    LOG( QObject::tr( "File %1 is not readable" ).arg( drawing ) );
    return false;
  }

  // Verify that the new drawing is newer than the previous one.
  sqlite3_stmt *stmt = nullptr;
  if ( sqlite3_prepare_v2( mDb, QString( "SELECT d,lastmodified FROM drawings WHERE path=%1" ).arg( quotedValue( fi.canonicalPath() ) ).toUtf8(), -1, &stmt, nullptr ) == SQLITE_OK )
  {
    if ( sqlite3_step( stmt ) == SQLITE_ROW )
    {
      mDrawing = QString::fromUtf8(( const char* ) sqlite3_column_text( stmt, 0 ) ).toInt();
      QDateTime lastModified = QDateTime::fromString( QString::fromUtf8(( const char* ) sqlite3_column_text( stmt, 1 ) ) );

      if ( fi.lastModified() <= lastModified )
      {
        LOG( QObject::tr( "File %1 already in database." ).arg( drawing ) );
        return false;
      }

      if ( !exec( QString( "DELETE FROM drawings WHERE d=%1" ).arg( mDrawing ) ) )
      {
        return false;
      }

      LOG( QObject::tr( "Replacing previous version of drawing from %1." ).arg( drawing ).arg( fi.lastModified().toString() ) );
    }
  }

  if ( !exec( QString( "INSERT INTO drawings(d,path,importedat,lastmodified) VALUES (%1,%2,%3,%3)" )
              .arg( mDrawing < 0 ? "NULL" : QString::number( mDrawing ) )
              .arg( quotedValue( fi.canonicalPath() ) )
              .arg( quotedValue( fi.lastModified().toString( Qt::ISODate ) ) ) ) )
  {
    LOG( QObject::tr( "Could not insert drawing %1." ).arg( drawing ) );
    return false;
  }

  if ( mDrawing < 0 )
  {
    mDrawing = sqlite3_last_insert_rowid( mDb );
  }

  if ( fi.suffix().toLower() == "dxf" )
  {
    //loads dxf
    QSharedPointer<dxfRW> dxf( new dxfRW( drawing.toUtf8() ) );
    return dxf->read( this, false );
  }
  else if ( fi.suffix().toLower() == "dwg" )
  {
    //loads dwg
    QSharedPointer<dwgR> dwg( new dwgR( drawing.toUtf8() ) );
    return dwg->read( this, false );
  }
  else
  {
    LOG( QObject::tr( "File %1 is not a DWG/DXF file" ).arg( drawing ) );
    return false;
  }
}

void QgsDwgImporter::addHeader( const DRW_Header *data )
{
  QgsDebugCall;

  if ( !data )
    return;

  if ( !data->getComments().empty() )
    exec( QString( "UPDATE drawings SET comments=%1 WHERE d=%2" ).arg( quotedValue( QString::fromStdString( data->getComments() ) ).arg( mDrawing ) ) );

  if ( data->vars.empty() )
    return;

  for ( std::map<std::string, DRW_Variant*>::const_iterator it = data->vars.begin(); it != data->vars.end(); ++it )
  {
    QString sql( "INSERT INTO headers(d,k,v) VALUES (%1,%2,%3)" );

    sql = sql.arg( mDrawing ).arg( quotedValue( QString::fromStdString( it->first ) ) );

    switch ( it->second->type() )
    {
      case DRW_Variant::STRING:
        sql = sql.arg( quotedValue( QString::fromStdString( *it->second->content.s ) ) );
        break;

      case DRW_Variant::INTEGER:
        sql = sql.arg( it->second->content.i );
        break;

      case DRW_Variant::DOUBLE:
        sql = sql.arg( qgsDoubleToString( it->second->content.d ) );
        break;

      case DRW_Variant::COORD:
        sql = sql.arg( QString( "'%1,%2,%3'" )
                       .arg( qgsDoubleToString( it->second->content.v->x ) )
                       .arg( qgsDoubleToString( it->second->content.v->y ) )
                       .arg( qgsDoubleToString( it->second->content.v->z ) ) );
        break;

      case DRW_Variant::INVALID:
        break;
    }

    if ( !exec( sql ) )
      break;
  }
}

void QgsDwgImporter::addLType( const DRW_LType &data )
{
  QgsDebugCall;

  QStringList path;
  Q_FOREACH ( const double &d, data.path )
  {
    path << qgsDoubleToString( d );
  }

  exec( QString( "INSERT INTO linetypes(d,name,desc,path) VALUES (%1,%2,%3,%4)" )
        .arg( mDrawing )
        .arg( quotedValue( QString::fromStdString( data.name ) ),
              quotedValue( QString::fromStdString( data.desc ) ),
              quotedValue( path.join( "," ) ) )
      );
}

void QgsDwgImporter::addLayer( const DRW_Layer &data )
{
  QgsDebugCall;

  exec( QString( "INSERT INTO layers(d,name,linetype,color,color24,lweight) VALUES (%1,%2,%3,%4,%5,%6)" )
        .arg( mDrawing )
        .arg( quotedValue( QString::fromStdString( data.name ) ) )
        .arg( quotedValue( QString::fromStdString( data.lineType ) ) )
        .arg( data.color )
        .arg( data.color24 )
        .arg( DRW_LW_Conv::lineWidth2dxfInt( data.lWeight ) ) );
}

void QgsDwgImporter::addDimStyle( const DRW_Dimstyle& data )
{
  QgsDebugCall;

  exec( QString( "INSERT INTO dimstyles(d,name,dimpost,dimapost,dimblk,dimblk1,dimblk2,dimscale,dimasz,dimexo,dimdli,dimexe,dimrnd,dimdle,dimtp,dimtm,dimfxl,dimtxt,dimcen,dimtsz,dimaltf,dimlfac,dimtvp,dimtfac,dimgap,dimaltrnd,dimtol,dimlim,dimtih,dimtoh,dimse1,dimse2,dimtad,dimzin,dimazin,dimalt,dimaltd,dimtofl,dimsah,dimtix,dimsoxd,dimclrd,dimclre,dimclrt,dimadec,dimunit,dimdec,dimtdec,dimaltu,dimalttd,dimaunit,dimfrac,dimlunit,dimdsep,dimtmove,dimjust,dimsd1,dimsd2,dimtolj,dimtzin,dimaltz,dimaltttz,dimfit,dimupt,dimatfit,dimfxlon,dimtxsty,dimldrblk,dimlwd,dimlwe) VALUES (%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17,%18,%19,%20,%21,%22,%23,%24,%25,%26,%27,%28,%29,%30,%31,%32,%33,%34,%35,%36,%37,%38,%39,%40,%41,%42,%43,%44,%45,%46,%47,%48,%49,%50,%51,%52,%53,%54,%55,%56,%57,%58,%59,%60,%61,%62,%63,%64,%65,%66,%67,%68,%69,%70)" )
        .arg( mDrawing )  // 1
        .arg( quotedValue( QString::fromStdString( data.name ) ) ) // 2
        .arg( quotedValue( QString::fromStdString( data.dimpost ) ) ) // 3
        .arg( quotedValue( QString::fromStdString( data.dimapost ) ) ) // 4
        .arg( quotedValue( QString::fromStdString( data.dimblk ) ) ) // 5
        .arg( quotedValue( QString::fromStdString( data.dimblk1 ) ) ) // 6
        .arg( quotedValue( QString::fromStdString( data.dimblk2 ) ) ) // 7
        .arg( qgsDoubleToString( data.dimscale ) ) // 8
        .arg( qgsDoubleToString( data.dimasz ) ) // 9
        .arg( qgsDoubleToString( data.dimexo ) ) // 10
        .arg( qgsDoubleToString( data.dimdli ) ) // 11
        .arg( qgsDoubleToString( data.dimexe ) ) // 12
        .arg( qgsDoubleToString( data.dimrnd ) ) // 13
        .arg( qgsDoubleToString( data.dimdle ) ) // 14
        .arg( qgsDoubleToString( data.dimtp ) ) // 15
        .arg( qgsDoubleToString( data.dimtm ) ) // 16
        .arg( qgsDoubleToString( data.dimfxl ) ) // 17
        .arg( qgsDoubleToString( data.dimtxt ) ) // 18
        .arg( qgsDoubleToString( data.dimcen ) ) // 19
        .arg( qgsDoubleToString( data.dimtsz ) ) // 20
        .arg( qgsDoubleToString( data.dimaltf ) ) // 21
        .arg( qgsDoubleToString( data.dimlfac ) ) // 22
        .arg( qgsDoubleToString( data.dimtvp ) ) // 23
        .arg( qgsDoubleToString( data.dimtfac ) ) // 24
        .arg( qgsDoubleToString( data.dimgap ) ) // 25
        .arg( qgsDoubleToString( data.dimaltrnd ) ) // 26
        .arg( data.dimtol ) // 27
        .arg( data.dimlim ) // 28
        .arg( data.dimtih ) // 29
        .arg( data.dimtoh ) // 30
        .arg( data.dimse1 ) // 31
        .arg( data.dimse2 ) // 32
        .arg( data.dimtad ) // 33
        .arg( data.dimzin ) // 34
        .arg( data.dimazin ) // 35
        .arg( data.dimalt ) // 36
        .arg( data.dimaltd ) // 37
        .arg( data.dimtofl ) // 38
        .arg( data. dimsah ) // 39
        .arg( data.dimtix ) // 40
        .arg( data.dimsoxd ) // 41
        .arg( data.dimclrd ) // 42
        .arg( data.dimclre ) // 43
        .arg( data.dimclrt ) // 44
        .arg( data.dimadec ) // 45
        .arg( data.dimunit ) // 46
        .arg( data.dimdec ) // 47
        .arg( data.dimtdec ) // 48
        .arg( data.dimaltu ) // 49
        .arg( data.dimalttd ) // 50
        .arg( data.dimaunit ) // 51
        .arg( data.dimfrac ) // 52
        .arg( data.dimlunit ) // 53
        .arg( data.dimdsep ) // 54
        .arg( data.dimtmove ) // 55
        .arg( data.dimjust ) // 56
        .arg( data.dimsd1 ) // 57
        .arg( data.dimsd2 ) // 58
        .arg( data.dimtolj ) // 59
        .arg( data.dimtzin ) // 60
        .arg( data.dimaltz ) // 61
        .arg( data.dimaltttz ) // 62
        .arg( data.dimfit ) // 63
        .arg( data.dimupt ) // 64
        .arg( data.dimatfit ) // 65
        .arg( data.dimfxlon ) // 66
        .arg( quotedValue( QString::fromStdString( data.dimtxsty ) ) ) // 67
        .arg( quotedValue( QString::fromStdString( data.dimldrblk ) ) ) // 68
        .arg( data.dimlwd ) // 69
        .arg( data.dimlwe ) // 70
      );
}

void QgsDwgImporter::addVport( const DRW_Vport &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addTextStyle( const DRW_Textstyle& data )
{
  QgsDebugCall;

  exec( QString( "INSERT INTO textstyles(d,name,height,width,oblique,genFlag,lastheight,font,bigFont,fontfamily) VALUES (%1,%2,%3,%4,%5,%6,%7,%8,%9,%10)" )
        .arg( mDrawing )
        .arg( quotedValue( QString::fromStdString( data.name ) ) )
        .arg( qgsDoubleToString( data.height ) )
        .arg( qgsDoubleToString( data.width ) )
        .arg( qgsDoubleToString( data.oblique ) )
        .arg( data.genFlag )
        .arg( qgsDoubleToString( data.lastHeight ) )
        .arg( quotedValue( QString::fromStdString( data.font ) ) )
        .arg( quotedValue( QString::fromStdString( data.bigFont ) ) )
        .arg( data.fontFamily )
      );
}

void QgsDwgImporter::addAppId( const DRW_AppId& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addBlock( const DRW_Block& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::setBlock( const int handle )
{
  Q_UNUSED( handle );
  QgsDebugCall;
}

void QgsDwgImporter::endBlock()
{
  QgsDebugCall;
}

void QgsDwgImporter::addEntity( const DRW_Entity &data )
{
  QgsDebugCall;

  exec( QString( "INSERT INTO entities(d,handle,etype,space,layer,linetype,color,color24,transparency,lweight,ltscale,visible) VALUES (%1,%2,%3,%4,%5,%6,%7,%8,%9,%11,%12" )
        .arg( mDrawing )
        .arg( data.handle )
        .arg( data.eType )
        .arg( data.space )
        .arg( quotedValue( QString::fromStdString( data.layer ) ) )
        .arg( quotedValue( QString::fromStdString( data.lineType ) ) )
        .arg( data.color )
        .arg( data.color24 )
        .arg( data.transparency )
        .arg( DRW_LW_Conv::lineWidth2dxfInt( data.lWeight ) )
        .arg( qgsDoubleToString( data.ltypeScale ) )
        .arg( data.visible )
      );
}

void QgsDwgImporter::addPoint( const DRW_Point& data )
{
  QgsDebugCall;

  addEntity( data );

  exec( QString( "INSERT INTO points(d,handle,geom,thickness,extPoint) VALUES (%1,%2,MakePoint(%3,%4,%5),%6,MakePoint(%7,%8,%9)" )
        .arg( mDrawing )
        .arg( data.handle )
        .arg( qgsDoubleToString( data.basePoint.x ) ).arg( qgsDoubleToString( data.basePoint.y ) ).arg( qgsDoubleToString( data.basePoint.z ) )
        .arg( data.thickness )
        .arg( qgsDoubleToString( data.extPoint.x ) ).arg( qgsDoubleToString( data.extPoint.y ) ).arg( qgsDoubleToString( data.extPoint.z ) )
      );
}

void QgsDwgImporter::addLine( const DRW_Line& data )
{
  QgsDebugCall;

  addEntity( data );

  exec( QString( "INSERT INTO lines(d,handle,geom,thickness,extPoint) VALUES (%1,%2,MakeLine(MakePoint(%3,%4,%5),MakePoint(%6,%7,%8)),%9,MakePoint(%10,%11,%12" )
        .arg( mDrawing )
        .arg( data.handle )
        .arg( qgsDoubleToString( data.basePoint.x ) ).arg( qgsDoubleToString( data.basePoint.y ) ).arg( qgsDoubleToString( data.basePoint.z ) )
        .arg( qgsDoubleToString( data.secPoint.x ) ).arg( qgsDoubleToString( data.secPoint.y ) ).arg( qgsDoubleToString( data.secPoint.z ) )
        .arg( data.thickness )
        .arg( qgsDoubleToString( data.extPoint.x ) ).arg( qgsDoubleToString( data.extPoint.y ) ).arg( qgsDoubleToString( data.extPoint.z ) )
      );
}

void QgsDwgImporter::addRay( const DRW_Ray& data )
{
  Q_UNUSED( data );
  NYI( QObject::tr( "RAY entities" ) );
}

void QgsDwgImporter::addXline( const DRW_Xline& data )
{
  QgsDebugCall;
  Q_UNUSED( data );
  NYI( QObject::tr( "XLINE entities" ) );
}

void QgsDwgImporter::addArc( const DRW_Arc& data )
{
  QgsDebugCall;
  NYI( QObject::tr( "ARC entities" ) );
}

void QgsDwgImporter::addCircle( const DRW_Circle& data )
{
  QgsDebugCall;
  NYI( QObject::tr( "CIRCLE entities" ) );
}

void QgsDwgImporter::addEllipse( const DRW_Ellipse& data )
{
  Q_UNUSED( data );
  NYI( QObject::tr( "ELLIPSE entities" ) );
}

void QgsDwgImporter::addLWPolyline( const DRW_LWPolyline& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "LWPOLYLINE entities" ) );
}

void QgsDwgImporter::addPolyline( const DRW_Polyline& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addSpline( const DRW_Spline* data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addKnot( const DRW_Entity& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addInsert( const DRW_Insert& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addTrace( const DRW_Trace& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::add3dFace( const DRW_3Dface& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addSolid( const DRW_Solid& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addMText( const DRW_MText& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addText( const DRW_Text& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimAlign( const DRW_DimAligned *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimLinear( const DRW_DimLinear *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimRadial( const DRW_DimRadial *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimDiametric( const DRW_DimDiametric *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimAngular( const DRW_DimAngular *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimAngular3P( const DRW_DimAngular3p *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addDimOrdinate( const DRW_DimOrdinate *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addLeader( const DRW_Leader *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addHatch( const DRW_Hatch *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addViewport( const DRW_Viewport& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addImage( const DRW_Image *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::linkImage( const DRW_ImageDef *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addComment( const char *comment )
{
  Q_UNUSED( comment );
  QgsDebugCall;
}

void QgsDwgImporter::writeHeader( DRW_Header& data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::writeBlocks()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeBlockRecords()
{
  QgsDebugCall;
}


void QgsDwgImporter::writeEntities()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeLTypes()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeLayers()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeTextstyles()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeVports()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeDimstyles()
{
  QgsDebugCall;
}

void QgsDwgImporter::writeAppId()
{
  QgsDebugCall;
}
