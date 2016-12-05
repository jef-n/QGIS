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
#include "qgis.h"
#include "qgspointv2.h"
#include "qgslinestringv2.h"
#include "qgscircularstringv2.h"
#include "qgscurvepolygonv2.h"
#include "qgscompoundcurvev2.h"
#include "qgspolygonv2.h"

#include <QString>
#include <QStringList>
#include <QFileInfo>
#include <QVector>
#include <typeinfo>

#include <cpl_port.h>
#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>

#ifdef QGISDEBUG
#include "intern/drw_dbg.h"
#endif

#define LOG( x ) { QgsDebugMsg( x ); QgsMessageLog::logMessage( x, QObject::tr( "DWG/DXF import" ) ); }
#define ONCE( x ) { static bool show=true; if( show ) LOG( x ); show=false; }
#define NYI( x ) { static bool show=true; if( show ) LOG( QObject::tr("Not yet implemented %1").arg( x ) ); show=false; }


QgsDwgImporter::QgsDwgImporter( const QString &database )
    : mDs( nullptr )
    , mDatabase( database )
    , mInTransaction( true )
    , mSplineSegs( 8 )
{
  QgsDebugCall;
}

bool QgsDwgImporter::exec( QString sql, bool logError )
{
  if ( !mDs )
  {
    QgsDebugMsg( "No data source" );
    return false;
  }

  CPLErrorReset();

  OGRLayerH layer = OGR_DS_ExecuteSQL( mDs, sql.toUtf8().constData(), nullptr, nullptr );
  if ( layer )
  {
    QgsDebugMsg( "Unexpected result set" );
    OGR_DS_ReleaseResultSet( mDs, layer );
    return false;
  }

  if ( CPLGetLastErrorType() == CE_None )
    return true;

  if ( logError )
  {
    LOG( QObject::tr( "SQL statement failed\nDatabase:%1\nSQL:%2\nError:%3" )
         .arg( mDatabase )
         .arg( sql )
         .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
  return false;
}

OGRLayerH QgsDwgImporter::query( QString sql )
{
  if ( !mDs )
  {
    QgsDebugMsg( "No data source" );
    return nullptr;
  }

  CPLErrorReset();

  OGRLayerH layer = OGR_DS_ExecuteSQL( mDs, sql.toUtf8().constData(), nullptr, nullptr );
  if ( !layer )
  {
    QgsDebugMsg( "Result expected" );
    return layer;
  }

  if ( CPLGetLastErrorType() == CE_None )
    return layer;

  LOG( QObject::tr( "SQL statement failed\nDatabase:%1\nSQL:%2\nError:%3" )
       .arg( mDatabase )
       .arg( sql )
       .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  OGR_DS_ReleaseResultSet( mDs, layer );

  return nullptr;
}


void QgsDwgImporter::startTransaction()
{
  Q_ASSERT( mDs );

  mInTransaction = GDALDatasetStartTransaction( mDs, 0 ) == OGRERR_NONE;

  if ( !mInTransaction )
  {
    LOG( QObject::tr( "Could not start transaction\nDatabase:%1\nError:%2" )
         .arg( mDatabase )
         .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::commitTransaction()
{
  Q_ASSERT( mDs != nullptr );

  if ( mInTransaction && GDALDatasetCommitTransaction( mDs ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not commit transaction\nDatabase:%1\nError:%2" )
         .arg( mDatabase )
         .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
  mInTransaction = false;
}

QgsDwgImporter::~QgsDwgImporter()
{
  QgsDebugCall;


  if ( mDs )
  {
    commitTransaction();

    OGR_DS_Destroy( mDs );
    mDs = nullptr;
  }
}

bool QgsDwgImporter::import( const QString &drawing )
{
  QgsDebugCall;

  QFileInfo fi( drawing );
  if ( !fi.isReadable() )
  {
    LOG( QObject::tr( "Drawing %1 is unreadable" ).arg( drawing ) );
    return false;
  }

  if ( QFileInfo( mDatabase ).exists() )
  {
    mDs = OGROpen( mDatabase.toUtf8().constData(), true, nullptr );
    if ( !mDs )
    {
      LOG( QObject::tr( "Could not open database [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      return false;
    }

    // Check whether database is uptodate
    OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "drawing" );
    if ( !layer )
    {
      LOG( QObject::tr( "Query for drawing %1 failed." ).arg( drawing ) );
      OGR_DS_Destroy( mDs );
      mDs = nullptr;
      return false;
    }

    OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
    int pathIdx = OGR_FD_GetFieldIndex( dfn, "path" );
    int lastmodifiedIdx = OGR_FD_GetFieldIndex( dfn, "lastmodified" );

    OGR_L_ResetReading( layer );

    OGRFeatureH f = OGR_L_GetNextFeature( layer );
    if ( !f )
    {
      LOG( QObject::tr( "Could not retrieve drawing name from database [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      OGR_DS_Destroy( mDs );
      mDs = nullptr;
      return false;
    }

    QString path = QString::fromUtf8( OGR_F_GetFieldAsString( f, pathIdx ) );

    int year, month, day, hour, minute, second, tzf;
    if ( !OGR_F_GetFieldAsDateTime( f, lastmodifiedIdx, &year, &month, &day, &hour, &minute, &second, &tzf ) )
    {
      LOG( QObject::tr( "Recorded last modification date unreadable [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      OGR_F_Destroy( f );
      OGR_DS_Destroy( mDs );
      mDs = nullptr;
      return false;
    }

    QDateTime lastModified( QDate( year, month, day ), QTime( hour, minute, second ) );

    if ( path == fi.canonicalPath() && fi.lastModified() <= lastModified )
    {
      LOG( QObject::tr( "Drawing already uptodate in database." ) );
      OGR_F_Destroy( f );
      return true;
    }

    OGR_DS_Destroy( mDs );
    mDs = nullptr;

    QFile::remove( mDatabase );
  }

  struct field
  {
    field( QString name, OGRFieldType ogrType, int width = -1, int precision = -1 )
        : mName( name ), mOgrType( ogrType ), mWidth( width ), mPrecision( precision )
    {}

    QString mName;
    OGRFieldType mOgrType;
    int mWidth;
    int mPrecision;
  };

  struct table
  {
    table( QString name, QString desc, OGRwkbGeometryType wkbType, QList<field> fields )
        : mName( name ), mDescription( desc ), mWkbType( wkbType ), mFields( fields )
    {}

    QString mName;
    QString mDescription;
    OGRwkbGeometryType mWkbType;
    QList<field> mFields;
  };

#define ENTITY_ATTRIBUTES \
  << field( "handle", OFTInteger ) \
  << field( "etype", OFTInteger ) \
  << field( "space", OFTInteger ) \
  << field( "layer", OFTString ) \
  << field( "linetype", OFTString ) \
  << field( "color", OFTString ) \
  << field( "lweight", OFTInteger ) \
  << field( "ltscale", OFTReal ) \
  << field( "visible", OFTInteger )

  QList<table> tables = QList<table>()
                        << table( "drawing", QObject::tr( "Imported drawings" ), wkbNone, QList<field>()
                                  << field( "path", OFTString )
                                  << field( "comments", OFTString )
                                  << field( "importdat", OFTDateTime )
                                  << field( "lastmodified", OFTDateTime )
                                )
                        << table( "headers", QObject::tr( "Headers" ), wkbNone, QList<field>()
                                  << field( "k", OFTString )
                                  << field( "v", OFTString )
                                )
                        << table( "linetypes", QObject::tr( "Line types" ), wkbNone, QList<field>()
                                  << field( "name", OFTString )
                                  << field( "desc", OFTString )
                                  << field( "path", OFTRealList )
                                )
                        << table( "layers", QObject::tr( "Layer list" ), wkbNone, QList<field>()
                                  << field( "name", OFTString )
                                  << field( "linetype", OFTString )
                                  << field( "color", OFTString )
                                  << field( "lweight", OFTInteger )
                                )
                        << table( "dimstyles", QObject::tr( "Dimension styles" ), wkbNone, QList<field>()
                                  << field( "name", OFTString )
                                  << field( "dimpost", OFTString )
                                  << field( "dimapost", OFTString )
                                  << field( "dimblk", OFTString )
                                  << field( "dimblk1", OFTString )
                                  << field( "dimblk2", OFTString )
                                  << field( "dimscale", OFTReal )
                                  << field( "dimasz", OFTReal )
                                  << field( "dimexo", OFTReal )
                                  << field( "dimdli", OFTReal )
                                  << field( "dimexe", OFTReal )
                                  << field( "dimrnd", OFTReal )
                                  << field( "dimdle", OFTReal )
                                  << field( "dimtp", OFTReal )
                                  << field( "dimtm", OFTReal )
                                  << field( "dimfxl", OFTReal )
                                  << field( "dimtxt", OFTReal )
                                  << field( "dimcen", OFTReal )
                                  << field( "dimtsz", OFTReal )
                                  << field( "dimaltf", OFTReal )
                                  << field( "dimlfac", OFTReal )
                                  << field( "dimtvp", OFTReal )
                                  << field( "dimtfac", OFTReal )
                                  << field( "dimgap", OFTReal )
                                  << field( "dimaltrnd", OFTReal )
                                  << field( "dimtol", OFTInteger )
                                  << field( "dimlim", OFTInteger )
                                  << field( "dimtih", OFTInteger )
                                  << field( "dimtoh", OFTInteger )
                                  << field( "dimse1", OFTInteger )
                                  << field( "dimse2", OFTInteger )
                                  << field( "dimtad", OFTInteger )
                                  << field( "dimzin", OFTInteger )
                                  << field( "dimazin", OFTInteger )
                                  << field( "dimalt", OFTInteger )
                                  << field( "dimaltd", OFTInteger )
                                  << field( "dimtofl", OFTInteger )
                                  << field( "dimsah", OFTInteger )
                                  << field( "dimtix", OFTInteger )
                                  << field( "dimsoxd", OFTInteger )
                                  << field( "dimclrd", OFTInteger )
                                  << field( "dimclre", OFTInteger )
                                  << field( "dimclrt", OFTInteger )
                                  << field( "dimadec", OFTInteger )
                                  << field( "dimunit", OFTInteger )
                                  << field( "dimdec", OFTInteger )
                                  << field( "dimtdec", OFTInteger )
                                  << field( "dimaltu", OFTInteger )
                                  << field( "dimalttd", OFTInteger )
                                  << field( "dimaunit", OFTInteger )
                                  << field( "dimfrac", OFTInteger )
                                  << field( "dimlunit", OFTInteger )
                                  << field( "dimdsep", OFTInteger )
                                  << field( "dimtmove", OFTInteger )
                                  << field( "dimjust", OFTInteger )
                                  << field( "dimsd1", OFTInteger )
                                  << field( "dimsd2", OFTInteger )
                                  << field( "dimtolj", OFTInteger )
                                  << field( "dimtzin", OFTInteger )
                                  << field( "dimaltz", OFTInteger )
                                  << field( "dimaltttz", OFTInteger )
                                  << field( "dimfit", OFTInteger )
                                  << field( "dimupt", OFTInteger )
                                  << field( "dimatfit", OFTInteger )
                                  << field( "dimfxlon", OFTInteger )
                                  << field( "dimtxsty", OFTString )
                                  << field( "dimldrblk", OFTString )
                                  << field( "dimlwd", OFTInteger )
                                  << field( "dimlwe", OFTInteger )
                                )
                        << table( "textstyles", QObject::tr( "Text styles" ), wkbNone, QList<field>()
                                  << field( "name", OFTString )
                                  << field( "height", OFTReal )
                                  << field( "width", OFTReal )
                                  << field( "oblique", OFTReal )
                                  << field( "genFlag", OFTInteger )
                                  << field( "lastHeight", OFTReal )
                                  << field( "font", OFTString )
                                  << field( "bigFont", OFTString )
                                  << field( "fontFamily", OFTInteger )
                                )
                        << table( "appdata", QObject::tr( "Application data" ), wkbNone, QList<field>()
                                  << field( "handle", OFTInteger )
                                  << field( "i", OFTInteger )
                                  << field( "value", OFTString )
                                )
                        << table( "points", QObject::tr( "POINT entities" ), wkbPoint25D, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                )
                        << table( "circles", QObject::tr( "CIRCLE entities" ), wkbCircularString, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                )
                        << table( "lines", QObject::tr( "LINE entities" ), wkbLineString25D, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "ext", OFTRealList )
                                )
                        << table( "polylines", QObject::tr( "POLYLINE entities" ), wkbCompoundCurveZ, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "width", OFTReal )
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                )
                        << table( "texts", QObject::tr( "TEXT entities" ), wkbPoint25D, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                  << field( "height", OFTReal )
                                  << field( "text", OFTString )
                                  << field( "angle", OFTReal )
                                  << field( "widthscale", OFTReal )
                                  << field( "oblique", OFTReal )
                                  << field( "style", OFTString )
                                  << field( "textgen", OFTInteger )
                                  << field( "alignh", OFTInteger )
                                  << field( "alignv", OFTInteger )
                                  << field( "interlin", OFTReal )
                                )
                        << table( "hatches", QObject::tr( "HATCH entities" ), wkbCurvePolygon, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                  << field( "name", OFTString )
                                  << field( "solid", OFTInteger )
                                  << field( "associative", OFTInteger )
                                  << field( "hstyle", OFTInteger )
                                  << field( "hpattern", OFTInteger )
                                  << field( "doubleflag", OFTInteger )
                                  << field( "angle", OFTReal )
                                  << field( "scale", OFTReal )
                                  << field( "deflines", OFTInteger )
                                )
                        << table( "inserts", QObject::tr( "INSERT entities" ), wkbPoint25D, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                  << field( "name", OFTString )
                                  << field( "xscale", OFTReal )
                                  << field( "yscale", OFTReal )
                                  << field( "zscale", OFTReal )
                                  << field( "angle", OFTReal )
                                  << field( "colcount", OFTReal )
                                  << field( "rowcount", OFTReal )
                                  << field( "colspace", OFTReal )
                                  << field( "rowspace", OFTReal )
                                )
                        ;

  OGRSFDriverH driver = OGRGetDriverByName( "GPKG" );
  if ( !driver )
  {
    LOG( QObject::tr( "Could not load geopackage driver" ) );
    return false;
  }

  // create database
  mDs = OGR_Dr_CreateDataSource( driver, mDatabase.toUtf8().constData(), nullptr );
  if ( !mDs )
  {
    LOG( QObject::tr( "Creation of datasource failed [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    return false;
  }

  startTransaction();

  Q_FOREACH ( const table &t, tables )
  {
    char **options = nullptr;
    options = CSLSetNameValue( options, "OVERWRITE", "YES" );
    options = CSLSetNameValue( options, "DESCRIPTION", t.mDescription.toUtf8().constData() );
    if ( t.mWkbType == wkbNone )
    {
      options = CSLSetNameValue( options, "SPATIAL_INDEX", "NO" );
    }

    OGRLayerH layer = OGR_DS_CreateLayer( mDs, t.mName.toUtf8().constData(), nullptr, t.mWkbType, options );

    CSLDestroy( options );
    options = nullptr;

    if ( !layer )
    {
      LOG( QObject::tr( "Creation of drawing layer %1 failed [%2]" ).arg( t.mName, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      OGR_DS_Destroy( mDs );
      mDs = nullptr;
      return false;
    }

    Q_FOREACH ( const field &f, t.mFields )
    {
      OGRFieldDefnH fld = OGR_Fld_Create( f.mName.toUtf8().constData(), f.mOgrType );
      if ( !fld )
      {
        LOG( QObject::tr( "Creation of field definition for %1.%2 failed [%3]" ).arg( t.mName, f.mName, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        OGR_DS_Destroy( mDs );
        mDs = nullptr;
        return false;
      }

      if ( f.mWidth >= 0 )
        OGR_Fld_SetWidth( fld, f.mWidth );
      if ( f.mPrecision >= 0 )
        OGR_Fld_SetPrecision( fld, f.mPrecision );

      OGRErr res = OGR_L_CreateField( layer, fld, true );
      OGR_Fld_Destroy( fld );

      if ( res != OGRERR_NONE )
      {
        LOG( QObject::tr( "Creation of field %1.%2 failed [%3]" ).arg( t.mName, f.mName, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        OGR_DS_Destroy( mDs );
        mDs = nullptr;
        return false;
      }
    }
  }

  commitTransaction();

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "drawing" );
  Q_ASSERT( layer );

  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  int pathIdx = OGR_FD_GetFieldIndex( dfn, "path" );
  int importdatIdx = OGR_FD_GetFieldIndex( dfn, "importdat" );
  int lastmodifiedIdx = OGR_FD_GetFieldIndex( dfn, "lastmodified" );

  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  OGR_F_SetFieldString( f, pathIdx, fi.canonicalPath().toUtf8().constData() );

  QDateTime d( fi.lastModified() );
  OGR_F_SetFieldDateTime( f, lastmodifiedIdx,
                          d.date().year(),
                          d.date().month(),
                          d.date().day(),
                          d.time().hour(),
                          d.time().minute(),
                          d.time().second(),
                          0
                        );

  d = QDateTime::currentDateTime();
  OGR_F_SetFieldDateTime( f, importdatIdx,
                          d.date().year(),
                          d.date().month(),
                          d.date().day(),
                          d.time().hour(),
                          d.time().minute(),
                          d.time().second(),
                          0
                        );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not update drawing record [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    OGR_F_Destroy( f );
    return false;
  }

  OGR_F_Destroy( f );

  LOG( QObject::tr( "Updating database from %1 [%2]." ).arg( drawing ).arg( fi.lastModified().toString() ) );

#ifdef QGISDEBUG
  DRW_DBGSL( DRW_dbg::DEBUG );
#endif

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

  Q_ASSERT( data );

  if ( !data->getComments().empty() )
  {
    OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "drawing" );
    OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
    int importdatIdx = OGR_FD_GetFieldIndex( dfn, "comments" );

    OGR_L_ResetReading( layer );
    OGRFeatureH f = OGR_L_GetNextFeature( layer );
    Q_ASSERT( f );

    OGR_F_SetFieldString( f, importdatIdx, QString::fromStdString( data->getComments() ).toUtf8().constData() );

    if ( OGR_L_SetFeature( layer, f ) != OGRERR_NONE )
    {
      LOG( QObject::tr( "Could not update comment in drawing record [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      OGR_F_Destroy( f );
      return;
    }

    OGR_F_Destroy( f );
  }

  if ( data->vars.empty() )
    return;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "headers" );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  int kIdx = OGR_FD_GetFieldIndex( dfn, "k" );
  int vIdx = OGR_FD_GetFieldIndex( dfn, "v" );

  for ( std::map<std::string, DRW_Variant*>::const_iterator it = data->vars.begin(); it != data->vars.end(); ++it )
  {
    OGRFeatureH f = OGR_F_Create( dfn );

    QString k = QString::fromStdString( it->first );

    QString v;
    switch ( it->second->type() )
    {
      case DRW_Variant::STRING:
        v = QString::fromStdString( *it->second->content.s );
        break;

      case DRW_Variant::INTEGER:
        v = QString::number( it->second->content.i );

        if ( k == "SPLINESEGS" )
          mSplineSegs = it->second->content.i;

        break;

      case DRW_Variant::DOUBLE:
        v = qgsDoubleToString( it->second->content.d );
        break;

      case DRW_Variant::COORD:
        v = QString( "%1,%2,%3" )
            .arg( qgsDoubleToString( it->second->content.v->x ) )
            .arg( qgsDoubleToString( it->second->content.v->y ) )
            .arg( qgsDoubleToString( it->second->content.v->z ) );
        break;

      case DRW_Variant::INVALID:
        break;
    }

    OGR_F_SetFieldString( f, kIdx, k.toUtf8().constData() );
    OGR_F_SetFieldString( f, vIdx, v.toUtf8().constData() );

    if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
    {
      LOG( QObject::tr( "Could not add header record %1 [%2]" ).arg( k, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    }

    OGR_F_Destroy( f );
  }
}

void QgsDwgImporter::addLType( const DRW_LType &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "linetypes" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );

  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "name" ), QString::fromStdString( data.name ).toUtf8().constData() );
  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "desc" ), QString::fromStdString( data.desc ).toUtf8().constData() );

  QVector<double> path( QVector<double>::fromStdVector( data.path ) );
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "path" ), path.size(), path.data() );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add line type %1 [%2]" ).arg( QString::fromStdString( data.name ), QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_Destroy( f );
}

QString QgsDwgImporter::colorString( int color, int color24, int transparency )
{
  if ( color24 == -1 )
  {
    return QString( "%1,%2,%3,%4" )
           .arg( DRW::dxfColors[ color ][0] )
           .arg( DRW::dxfColors[ color ][1] )
           .arg( DRW::dxfColors[ color ][2] )
           .arg( 255 - ( transparency & 0xff ) );
  }
  else
  {
    return QString( "%1,%2,%3,%4" )
           .arg(( color24 & 0xff0000 ) >> 16 )
           .arg(( color24 & 0x00ff00 ) >> 8 )
           .arg(( color24 & 0x0000ff ) )
           .arg( 255 - ( transparency & 0xff ) );
  }
}

void QgsDwgImporter::addLayer( const DRW_Layer &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "layers" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "name" ), QString::fromStdString( data.name ).toUtf8().constData() );
  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "linetype" ), QString::fromStdString( data.lineType ).toUtf8().constData() );
  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "color" ), colorString( data.color, data.color24, 0 ).toUtf8().constData() );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "lweight" ),  DRW_LW_Conv::lineWidth2dxfInt( data.lWeight ) );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add layer %1 [%2]" ).arg( QString::fromStdString( data.name ), QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_Destroy( f );
}

void QgsDwgImporter::addDimStyle( const DRW_Dimstyle &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "dimstyles" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data.a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )

  SETSTRING( name );
  SETSTRING( dimpost );
  SETSTRING( dimapost );
  SETSTRING( dimblk );
  SETSTRING( dimblk1 );
  SETSTRING( dimblk2 );
  SETDOUBLE( dimscale );
  SETDOUBLE( dimasz );
  SETDOUBLE( dimexo );
  SETDOUBLE( dimdli );
  SETDOUBLE( dimexe );
  SETDOUBLE( dimrnd );
  SETDOUBLE( dimdle );
  SETDOUBLE( dimtp );
  SETDOUBLE( dimtm );
  SETDOUBLE( dimfxl );
  SETDOUBLE( dimtxt );
  SETDOUBLE( dimcen );
  SETDOUBLE( dimtsz );
  SETDOUBLE( dimaltf );
  SETDOUBLE( dimlfac );
  SETDOUBLE( dimtvp );
  SETDOUBLE( dimtfac );
  SETDOUBLE( dimgap );
  SETDOUBLE( dimaltrnd );
  SETINTEGER( dimtol );
  SETINTEGER( dimlim );
  SETINTEGER( dimtih );
  SETINTEGER( dimtoh );
  SETINTEGER( dimse1 );
  SETINTEGER( dimse2 );
  SETINTEGER( dimtad );
  SETINTEGER( dimzin );
  SETINTEGER( dimazin );
  SETINTEGER( dimalt );
  SETINTEGER( dimaltd );
  SETINTEGER( dimtofl );
  SETINTEGER( dimsah );
  SETINTEGER( dimtix );
  SETINTEGER( dimsoxd );
  SETINTEGER( dimclrd );
  SETINTEGER( dimclre );
  SETINTEGER( dimclrt );
  SETINTEGER( dimadec );
  SETINTEGER( dimunit );
  SETINTEGER( dimdec );
  SETINTEGER( dimtdec );
  SETINTEGER( dimaltu );
  SETINTEGER( dimalttd );
  SETINTEGER( dimaunit );
  SETINTEGER( dimfrac );
  SETINTEGER( dimlunit );
  SETINTEGER( dimdsep );
  SETINTEGER( dimtmove );
  SETINTEGER( dimjust );
  SETINTEGER( dimsd1 );
  SETINTEGER( dimsd2 );
  SETINTEGER( dimtolj );
  SETINTEGER( dimtzin );
  SETINTEGER( dimaltz );
  SETINTEGER( dimaltttz );
  SETINTEGER( dimfit );
  SETINTEGER( dimupt );
  SETINTEGER( dimatfit );
  SETINTEGER( dimfxlon );
  SETSTRING( dimtxsty );
  SETSTRING( dimldrblk );
  SETINTEGER( dimlwd );
  SETINTEGER( dimlwe );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add layer %1 [%2]" ).arg( QString::fromStdString( data.name ), QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_Destroy( f );
}

void QgsDwgImporter::addVport( const DRW_Vport &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addTextStyle( const DRW_Textstyle &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "textstyles" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data.a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )

  SETSTRING( name );
  SETDOUBLE( height );
  SETDOUBLE( width );
  SETDOUBLE( oblique );
  SETINTEGER( genFlag );
  SETDOUBLE( lastHeight );
  SETSTRING( font );
  SETSTRING( bigFont );
  SETINTEGER( fontFamily );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER


  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add text style %1 [%2]" ).arg( QString::fromStdString( data.name ), QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_Destroy( f );
}

void QgsDwgImporter::addAppId( const DRW_AppId &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
}

void QgsDwgImporter::addBlock( const DRW_Block &data )
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

void QgsDwgImporter::addEntity( OGRFeatureDefnH dfn, OGRFeatureH f, const DRW_Entity &data )
{
  QgsDebugCall;

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data.a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )

  QgsDebugMsg( QString( "handle:0x%1" ).arg( data.handle, 0, 16 ) );
  SETINTEGER( handle );
  SETINTEGER( eType );
  SETINTEGER( space );
  SETSTRING( layer );
  SETSTRING( lineType );
  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "color" ), colorString( data.color, data.color24, data.transparency ).toUtf8().constData() );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "lweight" ), data.lWeight );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "ltscale" ), data.ltypeScale );
  SETINTEGER( visible );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER
}

void QgsDwgImporter::addPoint( const DRW_Point &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "points" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  QgsPointV2 p( QgsWKBTypes::PointZ, data.basePoint.x, data.basePoint.y, data.basePoint.z );
  int binarySize;
  unsigned char *wkb = p.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add point [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addRay( const DRW_Ray &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "RAY entities" ) );
}

void QgsDwgImporter::addXline( const DRW_Xline &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "XLINE entities" ) );
}

void QgsDwgImporter::addArc( const DRW_Arc &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "circles" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  double half = ( data.staangle + data.endangle ) / 2.0;
  if ( data.isccw )
    half += M_PI;

  QgsCircularStringV2 c;
  c.setPoints( QgsPointSequenceV2()
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + sin( data.staangle ) * data.mRadius, data.basePoint.y + cos( data.staangle ) * data.mRadius )
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + sin( half ) * data.mRadius, data.basePoint.y + cos( half ) * data.mRadius )
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + sin( data.endangle ) * data.mRadius, data.basePoint.y + cos( data.endangle ) * data.mRadius )
             );

  int binarySize;
  unsigned char *wkb = c.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add arc [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addCircle( const DRW_Circle &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "circles" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  QgsCircularStringV2 c;
  c.setPoints( QgsPointSequenceV2()
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x - data.mRadius, data.basePoint.y, data.basePoint.z )
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + data.mRadius, data.basePoint.y, data.basePoint.z )
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x - data.mRadius, data.basePoint.y, data.basePoint.z )
             );

  int binarySize;
  unsigned char *wkb = c.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add circle [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addEllipse( const DRW_Ellipse &data )
{
  QgsDebugCall;

  DRW_Polyline pol;
  data.toPolyline( &pol );
  addPolyline( pol );
}

bool QgsDwgImporter::curveFromLWPolyline( const DRW_LWPolyline &data, QgsCompoundCurveV2 &cc )
{
  int vertexnum = data.vertlist.size();
  if ( vertexnum == 0 )
  {
    QgsDebugMsg( "polyline without points" );
    return false;
  }

  QgsPointSequenceV2 s;
  bool hadBulge( data.vertlist[0]->bulge != 0.0 );
  std::vector<DRW_Vertex2D *>::size_type n = data.flags & 1 ? vertexnum + 1 : vertexnum;
  for ( std::vector<DRW_Vertex2D *>::size_type i = 0; i < n; i++ )
  {
    int i0 = i % vertexnum;

    Q_ASSERT( data.vertlist[i0] != nullptr );
    QgsDebugMsg( QString( "%1: %2,%3 bulge:%4" ).arg( i ).arg( data.vertlist[i0]->x ).arg( data.vertlist[i0]->y ).arg( data.vertlist[i0]->bulge ) );

    QgsPointV2 p( QgsWKBTypes::PointZ, data.vertlist[i0]->x, data.vertlist[i0]->y, data.elevation );
    s << p;

    bool hasBulge( data.vertlist[i0]->bulge != 0.0 );

    if ( hasBulge != hadBulge || i == n - 1 )
    {
      if ( hadBulge )
      {
        QgsCircularStringV2 *c = new QgsCircularStringV2();
        c->setPoints( s );
        cc.addCurve( c );
      }
      else
      {
        QgsLineStringV2 *c = new QgsLineStringV2();
        c->setPoints( s );
        cc.addCurve( c );
      }

      hadBulge = hasBulge;
      s.clear();
      s << p;
    }

    if ( hasBulge && i < n - 1 )
    {
      int i1 = ( i + 1 ) % vertexnum;

      double a = 2.0 * atan( data.vertlist[i]->bulge );
      double dx = data.vertlist[i1]->x - data.vertlist[i0]->x;
      double dy = data.vertlist[i1]->y - data.vertlist[i0]->y;
      double c = sqrt( dx * dx + dy * dy );
      double r = c / 2.0 / sin( a );
      double h = r * ( 1 - cos( a ) );

      s << QgsPointV2( QgsWKBTypes::PointZ,
                       data.vertlist[i0]->x + 0.5 * dx + h * dy / c,
                       data.vertlist[i0]->y + 0.5 * dy - h * dx / c,
                       data.elevation );
    }
  }

  return true;
}


void QgsDwgImporter::addLWPolyline( const DRW_LWPolyline &data )
{
  QgsDebugCall;

  int vertexnum = data.vertlist.size();
  if ( vertexnum == 0 )
  {
    QgsDebugMsg( "LWPolyline without vertices" );
    return;
  }

  QgsPointSequenceV2 s;
  QgsCompoundCurveV2 cc;
  double width;
  bool hadBulge( false );

  std::vector<DRW_Vertex2D *>::size_type n = data.flags & 1 ? vertexnum : vertexnum - 1;
  for ( std::vector<DRW_Vertex2D *>::size_type i = 0; i < n; i++ )
  {
    int i0 = i % vertexnum;
    int i1 = ( i + 1 ) % vertexnum;

    QgsPointV2 p0( QgsWKBTypes::PointZ, data.vertlist[i0]->x, data.vertlist[i0]->y, data.elevation );
    QgsPointV2 p1( QgsWKBTypes::PointZ, data.vertlist[i1]->x, data.vertlist[i1]->y, data.elevation );
    double staWidth = data.vertlist[i0]->stawidth == 0.0 ? data.width : data.vertlist[i0]->stawidth;
    double endWidth = data.vertlist[i0]->endwidth == 0.0 ? data.width : data.vertlist[i0]->endwidth;
    bool hasBulge( data.vertlist[i0]->bulge != 0.0 );

    QgsDebugMsg( QString( "i:%1,%2/%3 width=%4 staWidth=%5 endWidth=%6 hadBulge=%7 hasBulge=%8 l=%9 <=> %10" )
                 .arg( i0 ).arg( i1 ).arg( n )
                 .arg( width ).arg( staWidth ).arg( endWidth )
                 .arg( hadBulge ).arg( hasBulge )
                 .arg( p0.asWkt() )
                 .arg( p1.asWkt() )
               );

    if ( s.size() > 0 && ( width != staWidth || width != endWidth || hadBulge != hasBulge ) )
    {
      if ( hadBulge )
      {
        QgsCircularStringV2 *c = new QgsCircularStringV2();
        c->setPoints( s );
        QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
        cc.addCurve( c );
      }
      else
      {
        QgsLineStringV2 *c = new QgsLineStringV2();
        c->setPoints( s );
        QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
        cc.addCurve( c );
      }

      s.clear();

      if ( width != staWidth || width != endWidth )
      {
        // write out entity
        OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "polylines" );
        Q_ASSERT( layer );
        OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
        Q_ASSERT( dfn );
        OGRFeatureH f = OGR_F_Create( dfn );
        Q_ASSERT( f );

        addEntity( dfn, f, data );

        OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );
        OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "width" ), width );

        QVector<double> ext( 3 );
        ext[0] = data.extPoint.x;
        ext[1] = data.extPoint.y;
        ext[2] = data.extPoint.z;
        OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

        QgsDebugMsg( QString( "write curve:%1" ).arg( cc.asWkt() ) );

        int binarySize;
        unsigned char *wkb = cc.asWkb( binarySize );

        OGRGeometryH geom;
        if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
        {
          LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        }

        OGR_F_SetGeometryDirectly( f, geom );
        if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
        {
          LOG( QObject::tr( "Could not add linestring [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        }

        cc.clear();
      }
    }

    if ( staWidth == endWidth )
    {
      if ( s.size() == 0 )
      {
        s << p0;
        hadBulge = hasBulge;
        width = staWidth;
      }

      if ( hasBulge )
      {
        double a = 2.0 * atan( data.vertlist[i]->bulge );
        double dx = p1.x() - p0.x();
        double dy = p1.y() - p0.y();
        double c = sqrt( dx * dx + dy * dy );
        double r = c / 2.0 / sin( a );
        double h = r * ( 1 - cos( a ) );

        s << QgsPointV2( QgsWKBTypes::PointZ,
                         p0.x() + 0.5 * dx + h * dy / c,
                         p0.y() + 0.5 * dy - h * dx / c,
                         data.elevation );
      }

      s << p1;
    }
    else
    {
      OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "hatches" );
      Q_ASSERT( layer );
      OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
      Q_ASSERT( dfn );
      OGRFeatureH f = OGR_F_Create( dfn );
      Q_ASSERT( f );

      addEntity( dfn, f, data );

      OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

      QVector<double> ext( 3 );
      ext[0] = data.extPoint.x;
      ext[1] = data.extPoint.y;
      ext[2] = data.extPoint.z;
      OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

      QgsPoint ps( p0.x(), p0.y() );
      QgsPoint pe( p1.x(), p1.y() );
      QgsVector v(( pe - ps ).perpVector().normalized() );
      QgsVector vs( v * 0.5 * staWidth );
      QgsVector ve( v * 0.5 * endWidth );

      QgsPolygonV2 poly;
      QgsLineStringV2 *ls = new QgsLineStringV2();
      ls->setPoints( QgsPointSequenceV2()
                     << QgsPointV2( ps + vs )
                     << QgsPointV2( pe + ve )
                     << QgsPointV2( pe - ve )
                     << QgsPointV2( ps - vs )
                     << QgsPointV2( ps + vs )
                   );
      ls->addZValue( data.elevation );
      poly.setExteriorRing( ls );
      QgsDebugMsg( QString( "write poly:%1" ).arg( poly.asWkt() ) );

      int binarySize;
      unsigned char *wkb = poly.asWkb( binarySize );
      OGRGeometryH geom;
      if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
      {
        LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      }

      OGR_F_SetGeometryDirectly( f, geom );

      if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
      {
        LOG( QObject::tr( "Could not add polygon [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      }
    }
  }

  if ( s.size() > 0 )
  {
    if ( hadBulge )
    {
      QgsCircularStringV2 *c = new QgsCircularStringV2();
      c->setPoints( s );
      QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
      cc.addCurve( c );
    }
    else
    {
      QgsLineStringV2 *c = new QgsLineStringV2();
      c->setPoints( s );
      QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
      cc.addCurve( c );
    }
  }

  if ( cc.nCurves() > 0 )
  {
    // write out entity
    OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "polylines" );
    Q_ASSERT( layer );
    OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
    Q_ASSERT( dfn );
    OGRFeatureH f = OGR_F_Create( dfn );
    Q_ASSERT( f );

    addEntity( dfn, f, data );

    OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );
    OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "width" ), width );

    QVector<double> ext( 3 );
    ext[0] = data.extPoint.x;
    ext[1] = data.extPoint.y;
    ext[2] = data.extPoint.z;
    OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

    QgsDebugMsg( QString( "write curve:%1" ).arg( cc.asWkt() ) );

    int binarySize;
    unsigned char *wkb = cc.asWkb( binarySize );

    OGRGeometryH geom;
    if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
    {
      LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    }

    OGR_F_SetGeometryDirectly( f, geom );
    if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
    {
      LOG( QObject::tr( "Could not add linestring [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    }
  }
}

void QgsDwgImporter::addPolyline( const DRW_Polyline &data )
{
  QgsDebugCall;

  int vertexnum = data.vertlist.size();
  if ( vertexnum == 0 )
  {
    QgsDebugMsg( "Polyline without vertices" );
    return;
  }

  QgsPointSequenceV2 s;
  QgsCompoundCurveV2 cc;
  double width;
  bool hadBulge( false );

  std::vector<DRW_Vertex *>::size_type n = data.flags & 1 ? vertexnum : vertexnum - 1;
  for ( std::vector<DRW_Vertex *>::size_type i = 0; i < n; i++ )
  {
    int i0 = i % vertexnum;
    int i1 = ( i + 1 ) % vertexnum;

    QgsPointV2 p0( QgsWKBTypes::PointZ, data.vertlist[i0]->basePoint.x, data.vertlist[i0]->basePoint.y, data.vertlist[i0]->basePoint.z );
    QgsPointV2 p1( QgsWKBTypes::PointZ, data.vertlist[i1]->basePoint.x, data.vertlist[i1]->basePoint.y, data.vertlist[i1]->basePoint.z );
    double staWidth = data.vertlist[i0]->endwidth == 0.0 ? data.defendwidth : data.vertlist[i0]->stawidth;
    double endWidth = data.vertlist[i0]->stawidth == 0.0 ? data.defstawidth : data.vertlist[i0]->endwidth;
    bool hasBulge( data.vertlist[i0]->bulge != 0.0 );

    QgsDebugMsg( QString( "i:%1,%2/%3 width=%4 staWidth=%5 endWidth=%6 hadBulge=%7 hasBulge=%8 l=%9 <=> %10" )
                 .arg( i0 ).arg( i1 ).arg( n )
                 .arg( width ).arg( staWidth ).arg( endWidth )
                 .arg( hadBulge ).arg( hasBulge )
                 .arg( p0.asWkt() )
                 .arg( p1.asWkt() )
               );

    if ( s.size() > 0 && ( width != staWidth || width != endWidth || hadBulge != hasBulge ) )
    {
      if ( hadBulge )
      {
        QgsCircularStringV2 *c = new QgsCircularStringV2();
        c->setPoints( s );
        QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
        cc.addCurve( c );
      }
      else
      {
        QgsLineStringV2 *c = new QgsLineStringV2();
        c->setPoints( s );
        QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
        cc.addCurve( c );
      }

      s.clear();

      if ( width != staWidth || width != endWidth )
      {
        // write out entity
        OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "polylines" );
        Q_ASSERT( layer );
        OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
        Q_ASSERT( dfn );
        OGRFeatureH f = OGR_F_Create( dfn );
        Q_ASSERT( f );

        addEntity( dfn, f, data );

        OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );
        OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "width" ), width );

        QVector<double> ext( 3 );
        ext[0] = data.extPoint.x;
        ext[1] = data.extPoint.y;
        ext[2] = data.extPoint.z;
        OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

        QgsDebugMsg( QString( "write curve:%1" ).arg( cc.asWkt() ) );

        int binarySize;
        unsigned char *wkb = cc.asWkb( binarySize );

        OGRGeometryH geom;
        if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
        {
          LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        }

        OGR_F_SetGeometryDirectly( f, geom );
        if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
        {
          LOG( QObject::tr( "Could not add linestring [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
        }

        cc.clear();
      }
    }

    if ( staWidth == endWidth )
    {
      if ( s.size() == 0 )
      {
        s << p0;
        hadBulge = hasBulge;
        width = staWidth;
      }

      if ( hasBulge )
      {
        double a = 2.0 * atan( data.vertlist[i]->bulge );
        double dx = p1.x() - p0.x();
        double dy = p1.y() - p0.y();
        double dz = p1.z() - p0.z();
        double c = sqrt( dx * dx + dy * dy );
        double r = c / 2.0 / sin( a );
        double h = r * ( 1 - cos( a ) );

        s << QgsPointV2( QgsWKBTypes::PointZ,
                         p0.x() + 0.5 * dx + h * dy / c,
                         p0.y() + 0.5 * dy - h * dx / c,
                         p0.z() + 0.5 * dz );
      }

      s << p1;
    }
    else
    {
      OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "hatches" );
      Q_ASSERT( layer );
      OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
      Q_ASSERT( dfn );
      OGRFeatureH f = OGR_F_Create( dfn );
      Q_ASSERT( f );

      addEntity( dfn, f, data );

      OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

      QVector<double> ext( 3 );
      ext[0] = data.extPoint.x;
      ext[1] = data.extPoint.y;
      ext[2] = data.extPoint.z;
      OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

      QgsPoint ps( p0.x(), p0.y() );
      QgsPoint pe( p1.x(), p1.y() );
      QgsVector v(( pe - ps ).perpVector().normalized() );
      QgsVector vs( v * 0.5 * staWidth );
      QgsVector ve( v * 0.5 * endWidth );

      QgsPolygonV2 poly;
      QgsLineStringV2 *ls = new QgsLineStringV2();
      QgsPointSequenceV2 s;
      s << QgsPointV2( ps + vs );
      s.last().addZValue( p0.z() );
      s << QgsPointV2( pe + ve );
      s.last().addZValue( p1.z() );
      s << QgsPointV2( pe - ve );
      s.last().addZValue( p1.z() );
      s << QgsPointV2( ps - vs );
      s.last().addZValue( p0.z() );
      s << QgsPointV2( ps + vs );
      s.last().addZValue( p0.z() );
      ls->setPoints( s );
      poly.setExteriorRing( ls );
      QgsDebugMsg( QString( "write poly:%1" ).arg( poly.asWkt() ) );

      int binarySize;
      unsigned char *wkb = poly.asWkb( binarySize );
      OGRGeometryH geom;
      if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
      {
        LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      }

      OGR_F_SetGeometryDirectly( f, geom );

      if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
      {
        LOG( QObject::tr( "Could not add polygon [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
      }
    }
  }

  if ( s.size() > 0 )
  {
    if ( hadBulge )
    {
      QgsCircularStringV2 *c = new QgsCircularStringV2();
      c->setPoints( s );
      QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
      cc.addCurve( c );
    }
    else
    {
      QgsLineStringV2 *c = new QgsLineStringV2();
      c->setPoints( s );
      QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
      cc.addCurve( c );
    }
  }

  if ( cc.nCurves() > 0 )
  {
    // write out entity
    OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "polylines" );
    Q_ASSERT( layer );
    OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
    Q_ASSERT( dfn );
    OGRFeatureH f = OGR_F_Create( dfn );
    Q_ASSERT( f );

    addEntity( dfn, f, data );

    OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );
    OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "width" ), width );

    QVector<double> ext( 3 );
    ext[0] = data.extPoint.x;
    ext[1] = data.extPoint.y;
    ext[2] = data.extPoint.z;
    OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

    QgsDebugMsg( QString( "write curve:%1" ).arg( cc.asWkt() ) );

    int binarySize;
    unsigned char *wkb = cc.asWkb( binarySize );

    OGRGeometryH geom;
    if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
    {
      LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    }

    OGR_F_SetGeometryDirectly( f, geom );
    if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
    {
      LOG( QObject::tr( "Could not add linestring [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
    }
  }
}

void QgsDwgImporter::addSpline( const DRW_Spline *data )
{
  QgsDebugCall;

  Q_ASSERT( data );
  Q_ASSERT( data->ncontrol == ( int ) data->controllist.size() );

  QgsPointSequenceV2 in;
  for ( std::vector<DRW_Coord *>::size_type i = 0; i < data->controllist.size(); i++ )
  {
    const DRW_Coord &p = *data->controllist[i];
    in << QgsPointV2( QgsWKBTypes::PointZ, p.x, p.y, p.z );
  }

  QgsPointSequenceV2 out = getStrokePoints( in, data->flags );

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "polylines" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, *data );

  QgsLineStringV2 l;
  l.setPoints( out );

  int binarySize;
  unsigned char *wkb = l.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add spline [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addKnot( const DRW_Entity &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "KNOT entities" ) );
}

void QgsDwgImporter::addInsert( const DRW_Insert &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "inserts" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data.a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )

  SETSTRING( name );
  SETDOUBLE( xscale );
  SETDOUBLE( yscale );
  SETDOUBLE( zscale );
  SETDOUBLE( angle );
  SETINTEGER( colcount );
  SETINTEGER( rowcount );
  SETDOUBLE( colspace );
  SETDOUBLE( rowspace );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER

  QgsPointV2 p( QgsWKBTypes::PointZ, data.basePoint.x, data.basePoint.y, data.basePoint.z );
  int binarySize;
  unsigned char *wkb = p.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add point [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addTrace( const DRW_Trace &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "TRACE entities" ) );
}

void QgsDwgImporter::add3dFace( const DRW_3Dface &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "3DFACE entities" ) );
}

void QgsDwgImporter::addSolid( const DRW_Solid &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "hatches" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, "hpattern" ), "SOLID" );

  QgsPolygonV2 poly;

  // pt1 pt2
  // pt3 pt4
  QgsPointSequenceV2 s;
  s << QgsPointV2( QgsWKBTypes::PointZ,   data.basePoint.x,   data.basePoint.y, data.basePoint.z );
  s << QgsPointV2( QgsWKBTypes::PointZ,    data.secPoint.x,    data.secPoint.y, data.basePoint.z );
  s << QgsPointV2( QgsWKBTypes::PointZ, data.fourthPoint.x, data.fourthPoint.y, data.basePoint.z );
  s << QgsPointV2( QgsWKBTypes::PointZ,  data.thirdPoint.x,  data.thirdPoint.y, data.basePoint.z );
  s << s[0];

  QgsLineStringV2 *ls = new QgsLineStringV2();
  ls->setPoints( s );
  poly.setExteriorRing( ls );

  int binarySize;
  unsigned char *wkb = poly.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add polygon [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addMText( const DRW_MText &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "texts" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data.a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )

  SETDOUBLE( height );
  SETSTRING( text );
  SETDOUBLE( angle );
  SETDOUBLE( widthscale );
  SETDOUBLE( oblique );
  SETSTRING( style );
  SETINTEGER( textgen );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "alignh" ), data.alignH );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "alignv" ), data.alignV );
  SETDOUBLE( thickness );
  SETDOUBLE( interlin );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  QgsPointV2 p( QgsWKBTypes::PointZ, data.basePoint.x, data.basePoint.y, data.basePoint.z );

  int binarySize;
  unsigned char *wkb = p.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add line [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addText( const DRW_Text &data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "texts" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data.a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data.a )

  SETDOUBLE( height );
  SETSTRING( text );
  SETDOUBLE( angle );
  SETDOUBLE( widthscale );
  SETDOUBLE( oblique );
  SETSTRING( style );
  SETINTEGER( textgen );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "alignh" ), data.alignH );
  OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, "alignv" ), data.alignV );
  SETDOUBLE( thickness );
  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "interlin" ), -1.0 );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  QgsPointV2 p( QgsWKBTypes::PointZ, data.basePoint.x, data.basePoint.y, data.basePoint.z );

  int binarySize;
  unsigned char *wkb = p.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add line [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addDimAlign( const DRW_DimAligned *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "DIMALIGN entities" ) );
}

void QgsDwgImporter::addDimLinear( const DRW_DimLinear *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "DIMLINEAR entities" ) );
}

void QgsDwgImporter::addDimRadial( const DRW_DimRadial *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "DIMRADIAL entities" ) );
}

void QgsDwgImporter::addDimDiametric( const DRW_DimDiametric *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "DIMDIAMETRIC entities" ) );
}

void QgsDwgImporter::addDimAngular( const DRW_DimAngular *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "DIMANGULAR entities" ) );
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
  NYI( QObject::tr( "DIMORDINAL entities" ) );
}

void QgsDwgImporter::addLeader( const DRW_Leader *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "LEADER entities" ) );
}

void QgsDwgImporter::addHatch( const DRW_Hatch *data )
{
  Q_ASSERT( data != nullptr );
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "hatches" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, *data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data->thickness );

  QVector<double> ext( 3 );
  ext[0] = data->extPoint.x;
  ext[1] = data->extPoint.y;
  ext[2] = data->extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

#define SETSTRING(a)  OGR_F_SetFieldString( f, OGR_FD_GetFieldIndex( dfn, #a ), QString::fromStdString( data->a ).toUtf8().constData() )
#define SETDOUBLE(a)  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, #a ), data->a )
#define SETINTEGER(a) OGR_F_SetFieldInteger( f, OGR_FD_GetFieldIndex( dfn, #a ), data->a )

  SETSTRING( name );
  SETINTEGER( solid );
  SETINTEGER( associative );
  SETINTEGER( hstyle );
  SETINTEGER( hpattern );
  SETINTEGER( doubleflag );
  SETDOUBLE( angle );
  SETDOUBLE( scale );
  SETINTEGER( deflines );

#undef SETSTRING
#undef SETDOUBLE
#undef SETINTEGER

  QgsCurvePolygonV2 p;

  Q_ASSERT( data->looplist.size() == data->loopsnum );

  for ( std::vector<DRW_HatchLoop *>::size_type i = 0; i < data->loopsnum; i++ )
  {
    const DRW_HatchLoop &hatchLoop = *data->looplist[i];

    // TODO when do we get more than one hatch loop?
    Q_ASSERT( hatchLoop.objlist.size() == 1 );
    for ( std::vector<DRW_Entity *>::size_type j = 0;  j < hatchLoop.objlist.size(); j++ )
    {
      Q_ASSERT( hatchLoop.objlist[j] );
      const DRW_Entity *entity = hatchLoop.objlist[j];

      const DRW_LWPolyline *lwp = dynamic_cast<const DRW_LWPolyline *>( entity );
      if ( lwp )
      {
        QgsCompoundCurveV2 *cc = new QgsCompoundCurveV2();

        if ( curveFromLWPolyline( *lwp, *cc ) )
        {
          if ( i == 0 && j == 0 )
            p.setExteriorRing( cc );
          else
            p.addInteriorRing( cc );
        }
      }
      else
      {
        QgsDebugMsg( QString( "unknown obj %1.%2: %3" ).arg( i ).arg( j ).arg( typeid( *entity ).name() ) );
      }

      // entities
    }
  }

  int binarySize;
  unsigned char *wkb = p.asWkb( binarySize );
  OGRGeometryH geom;
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add polygon [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addLine( const DRW_Line& data )
{
  QgsDebugCall;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "lines" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  OGR_F_SetFieldDouble( f, OGR_FD_GetFieldIndex( dfn, "thickness" ), data.thickness );

  QVector<double> ext( 3 );
  ext[0] = data.extPoint.x;
  ext[1] = data.extPoint.y;
  ext[2] = data.extPoint.z;
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "ext" ), 3, ext.data() );

  QgsLineStringV2 l;

  l.setPoints( QgsPointSequenceV2()
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x, data.basePoint.y, data.basePoint.z )
               << QgsPointV2( QgsWKBTypes::PointZ, data.secPoint.x, data.secPoint.y, data.secPoint.z ) );

  OGRGeometryH geom;
  int binarySize;
  unsigned char *wkb = l.asWkb( binarySize );
  if ( OGR_G_CreateFromWkb( wkb, nullptr, &geom, binarySize ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not create geometry [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );

  }

  OGR_F_SetGeometryDirectly( f, geom );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add line [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::addViewport( const DRW_Viewport &data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "VIEWPORT entities" ) );
}

void QgsDwgImporter::addImage( const DRW_Image *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "IMAGE entities" ) );
}

void QgsDwgImporter::linkImage( const DRW_ImageDef *data )
{
  Q_UNUSED( data );
  QgsDebugCall;
  NYI( QObject::tr( "image links" ) );
}

void QgsDwgImporter::addComment( const char *comment )
{
  Q_UNUSED( comment );
  QgsDebugCall;
  NYI( QObject::tr( "comments" ) );
}

void QgsDwgImporter::writeHeader( DRW_Header &data )
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

QgsPointV2 QgsDwgImporter::getQuadPoint( const QgsPointV2& x1, const QgsPointV2& c1, const QgsPointV2& x2, double dt ) const
{
  return QgsPointV2(
           QgsWKBTypes::PointZ,
           x1.x()*( 1.0 - dt )*( 1.0 - dt ) + c1.x() * 2.0 * dt * ( 1.0 - dt ) + x2.x()*dt*dt,
           x1.y()*( 1.0 - dt )*( 1.0 - dt ) + c1.y() * 2.0 * dt * ( 1.0 - dt ) + x2.y()*dt*dt,
           x1.z()*( 1.0 - dt )*( 1.0 - dt ) + c1.z() * 2.0 * dt * ( 1.0 - dt ) + x2.z()*dt*dt
         );
}

int QgsDwgImporter::getQuadPoints( int iSeg, const QgsPointSequenceV2 &cps, bool closed, QgsPointV2 &start, QgsPointV2 &control, QgsPointV2 &end ) const
{
  int n = cps.size();

  int i1 = iSeg - 1;
  int i2 = iSeg;
  int i3 = iSeg + 1;

  if ( closed )
  {
    if ( n < 3 )
      return 0;

    i1 = ( i1 + n - 1 ) % n;
    i2--;
    i3 = ( i3 + n - 1 ) % n;

    start = QgsPointV2( QgsWKBTypes::PointZ,
                        ( cps.at( i1 ).x() + cps.at( i2 ).x() ) / 2.0,
                        ( cps.at( i1 ).y() + cps.at( i2 ).y() ) / 2.0,
                        ( cps.at( i1 ).z() + cps.at( i2 ).z() ) / 2.0 );

    control = cps.at( i2 );

    end = QgsPointV2( QgsWKBTypes::PointZ,
                      ( cps.at( i2 ).x() + cps.at( i3 ).x() ) / 2.0,
                      ( cps.at( i2 ).y() + cps.at( i3 ).y() ) / 2.0,
                      ( cps.at( i2 ).z() + cps.at( i3 ).z() ) / 2.0 );
  }
  else
  {
    if ( iSeg < 1 || n < 1 )
      return 0;

    start = cps.at( 0 );

    if ( n < 2 )
      return 1;

    end = cps.at( 1 );

    if ( n < 3 )
      return 2;

    control = end;
    end = cps.at( 2 );

    if ( n < 4 )
      return 3;

    start = i1 < 1 ? cps.at( 0 ) : QgsPointV2( QgsWKBTypes::PointZ,
            ( cps.at( i1 ).x() + cps.at( i2 ).x() ) / 2.0,
            ( cps.at( i1 ).y() + cps.at( i2 ).y() ) / 2.0,
            ( cps.at( i1 ).z() + cps.at( i2 ).z() ) / 2.0 );

    control = cps.at( i2 );

    end = i3 > n - 2 ?  cps.at( n - 1 ) : QgsPointV2( QgsWKBTypes::PointZ,
          ( cps.at( i2 ).x() + cps.at( i3 ).x() ) / 2.0,
          ( cps.at( i2 ).y() + cps.at( i3 ).y() ) / 2.0,
          ( cps.at( i2 ).z() + cps.at( i3 ).z() ) / 2.0 );
  }

  return 3;
}

void QgsDwgImporter::strokeQuad( QgsPointSequenceV2 &list, const QgsPointV2 &vx1, const QgsPointV2 &vc1, const QgsPointV2 &vx2 ) const
{
  if ( mSplineSegs < 1 )
  {
    list << vx1;
    return;
  }

  for ( int i = 0; i < mSplineSegs; i++ )
  {
    list << getQuadPoint( vx1, vc1, vx2, ( double ) i / ( double ) mSplineSegs );
  }
}

QgsPointSequenceV2 QgsDwgImporter::getStrokePoints( const QgsPointSequenceV2 &cps, bool closed ) const
{
  QgsPointSequenceV2 ps;

  int iSplines = ( int ) cps.size();
  if ( !closed )
    iSplines -= 2;

  QgsPointV2 end;
  for ( int i = 1; i <= iSplines; ++i )
  {
    QgsPointV2 start, control;
    int iPts = getQuadPoints( i, ps, closed, start, control, end );
    if ( iPts > 2 )
      strokeQuad( ps, start, control, end );
    else if ( iPts > 1 )
      ps << start;
  }

  if ( !closed && iSplines > 0 )
    ps << end;

  return ps;
}
