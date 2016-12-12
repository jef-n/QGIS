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

#define LOG( x ) { QgsDebugMsg( x ); QgsMessageLog::logMessage( x, QObject::tr( "DWG/DXF import" ) ); }
#define ONCE( x ) { static bool show=true; if( show ) LOG( x ); show=false; }
#define NYI( x ) { static bool show=true; if( show ) LOG( QObject::tr("Not yet implemented %1").arg( x ) ); show=false; }
#define SETSTRING(a)  setString(dfn, f, #a, data.a)
#define SETDOUBLE(a)  setDouble(dfn, f, #a, data.a)
#define SETINTEGER(a) setInteger(dfn, f, #a, data.a)


QgsDwgImporter::QgsDwgImporter( const QString &database )
    : mDs( nullptr )
    , mDatabase( database )
    , mInTransaction( false )
    , mSplineSegs( 8 )
    , mBlockHandle( -1 )
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

#if 0
    if ( path == fi.canonicalPath() && fi.lastModified() <= lastModified )
    {
      LOG( QObject::tr( "Drawing already uptodate in database." ) );
      OGR_F_Destroy( f );
      return true;
    }
#endif

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
  << field( "block", OFTInteger ) \
  << field( "etype", OFTInteger ) \
  << field( "space", OFTInteger ) \
  << field( "layer", OFTString ) \
  << field( "olinetype", OFTString ) \
  << field( "linetype", OFTString ) \
  << field( "color", OFTString ) \
  << field( "ocolor", OFTInteger) \
  << field( "color24", OFTInteger) \
  << field( "transparency", OFTInteger) \
  << field( "lweight", OFTInteger ) \
  << field( "linewidth", OFTReal ) \
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
                                  << field( "ocolor", OFTInteger )
                                  << field( "color24", OFTInteger )
                                  << field( "transparency", OFTInteger )
                                  << field( "lweight", OFTInteger )
                                  << field( "linewidth", OFTReal )
                                  << field( "flags", OFTInteger )
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
                        << table( "blocks", QObject::tr( "BLOCK entities" ), wkbPoint25D, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                  << field( "name", OFTString )
                                  << field( "flags", OFTInteger )
                                )
                        << table( "points", QObject::tr( "POINT entities" ), wkbPoint25D, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                )
                        << table( "lines", QObject::tr( "LINE entities" ), wkbCompoundCurveZ, QList<field>()
                                  ENTITY_ATTRIBUTES
                                  << field( "thickness", OFTReal )
                                  << field( "ext", OFTRealList )
                                  << field( "width", OFTReal )
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

    OGR_F_SetFieldString( f, importdatIdx, data->getComments().c_str() );

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

    QString k = it->first.c_str();

    QString v;
    switch ( it->second->type() )
    {
      case DRW_Variant::STRING:
        v = *it->second->content.s->c_str();
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

  SETSTRING( name );
  SETSTRING( desc );

  QVector<double> path( QVector<double>::fromStdVector( data.path ) );
  OGR_F_SetFieldDoubleList( f, OGR_FD_GetFieldIndex( dfn, "path" ), path.size(), path.data() );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add line type %1 [%2]" ).arg( data.name.c_str() ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_Destroy( f );
}

QString QgsDwgImporter::colorString( int color, int color24, int transparency, const std::string &layer ) const
{
  QgsDebugMsg( QString( "colorString(color=%1, color24=0x%2, transparency=0x%3 layer=%4" )
               .arg( color )
               .arg( color24, 0, 16 )
               .arg( transparency, 0, 16 )
               .arg( layer.c_str() ) );
  if ( color24 == -1 )
  {
    if ( color == 0 )
    {
      NYI( QObject::tr( "color by block" ) );
      return "0,0,0,255";
    }
    else if ( color == 256 )
    {
      return mLayerColor.value( layer.c_str(), "0,0,0,255" );
    }
    else
    {
      if ( color < 0 )
        color = -color;

      return QString( "%1,%2,%3,%4" )
             .arg( DRW::dxfColors[ color ][0] )
             .arg( DRW::dxfColors[ color ][1] )
             .arg( DRW::dxfColors[ color ][2] )
             .arg( 255 - ( transparency & 0xff ) );
    }
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

  SETSTRING( name );
  SETSTRING( lineType );
  SETINTEGER( flags );

  QString color = colorString( data.color, data.color24, data.transparency, "" );
  mLayerColor.insert( data.name.c_str(), color );

  double linewidth = lineWidth( data.lWeight, "" );
  mLayerLinewidth.insert( data.name.c_str(), linewidth );

  setInteger( dfn, f, "ocolor", data.color );
  SETINTEGER( color24 );
  SETINTEGER( transparency );
  setString( dfn, f, "color", color.toUtf8().constData() );
  setInteger( dfn, f, "lweight", DRW_LW_Conv::lineWidth2dxfInt( data.lWeight ) );
  setInteger( dfn, f, "linewidth", linewidth );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add layer %1 [%2]" ).arg( data.name.c_str(), QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  OGR_F_Destroy( f );
}

void QgsDwgImporter::setString( OGRFeatureDefnH dfn, OGRFeatureH f, QString field, const std::string &value ) const
{
  int idx = OGR_FD_GetFieldIndex( dfn, field.toLower().toUtf8().constData() );
  if ( idx < 0 )
  {
    LOG( QObject::tr( "Field %1 not found" ).arg( field ) );
    return;
  }
  OGR_F_SetFieldString( f, idx, value.c_str() );
}

void QgsDwgImporter::setDouble( OGRFeatureDefnH dfn, OGRFeatureH f, QString field, double value ) const
{
  int idx = OGR_FD_GetFieldIndex( dfn, field.toLower().toUtf8().constData() );
  if ( idx < 0 )
  {
    LOG( QObject::tr( "Field %1 not found" ).arg( field ) );
    return;
  }
  OGR_F_SetFieldDouble( f, idx, value );
}

void QgsDwgImporter::setInteger( OGRFeatureDefnH dfn, OGRFeatureH f, QString field, int value ) const
{
  int idx = OGR_FD_GetFieldIndex( dfn, field.toLower().toUtf8().constData() );
  if ( idx < 0 )
  {
    LOG( QObject::tr( "Field %1 not found" ).arg( field ) );
    return;
  }
  OGR_F_SetFieldInteger( f, idx, value );
}

void QgsDwgImporter::setPoint( OGRFeatureDefnH dfn, OGRFeatureH f, QString field, const DRW_Coord &p ) const
{
  QVector<double> ext( 3 );
  ext[0] = p.x;
  ext[1] = p.y;
  ext[2] = p.z;

  int idx = OGR_FD_GetFieldIndex( dfn, field.toLower().toUtf8().constData() );
  if ( idx < 0 )
  {
    LOG( QObject::tr( "Field %1 not found" ).arg( field ) );
    return;
  }

  OGR_F_SetFieldDoubleList( f, idx, 3, ext.data() );
}

double QgsDwgImporter::lineWidth( int lWeight, const std::string &layer ) const
{
  switch ( lWeight )
  {
    case 0:
      return 0.00;
    case 1:
      return 0.05;
    case 2:
      return 0.09;
    case 3:
      return 0.13;
    case 4:
      return 0.15;
    case 5:
      return 0.18;
    case 6:
      return 0.20;
    case 7:
      return 0.25;
    case 8:
      return 0.30;
    case 9:
      return 0.35;
    case 10:
      return 0.40;
    case 11:
      return 0.50;
    case 12:
      return 0.53;
    case 13:
      return 0.60;
    case 14:
      return 0.70;
    case 15:
      return 0.80;
    case 16:
      return 0.90;
    case 17:
      return 1.00;
    case 18:
      return 1.06;
    case 19:
      return 1.20;
    case 20:
      return 1.40;
    case 21:
      return 1.58;
    case 22:
      return 2.00;
    case 23:
      return 2.11;
    case 29:
      return mLayerLinewidth.value( layer.c_str(), 0.0 );
    case 30:
      NYI( QObject::tr( "Line width by block" ) );
      return 0.0;
    case 31:
    default:
      NYI( QObject::tr( "Line width default" ) );
      return 0.0;
  }
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

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add layer %1 [%2]" ).arg( data.name.c_str() ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
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

  SETSTRING( name );
  SETDOUBLE( height );
  SETDOUBLE( width );
  SETDOUBLE( oblique );
  SETINTEGER( genFlag );
  SETDOUBLE( lastHeight );
  SETSTRING( font );
  SETSTRING( bigFont );
  SETINTEGER( fontFamily );

  if ( OGR_L_CreateFeature( layer, f ) != OGRERR_NONE )
  {
    LOG( QObject::tr( "Could not add add text style %1 [%2]" ).arg( data.name.c_str() ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
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
  QgsDebugCall;

  Q_ASSERT( mBlockHandle < 0 );
  mBlockHandle = data.handle;
  QgsDebugMsg( QString( "block %1/0x%2 starts" ).arg( data.name.c_str() ).arg( mBlockHandle, 0, 16 ) );

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "blocks" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  SETSTRING( name );
  SETINTEGER( flags );

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
    LOG( QObject::tr( "Could not add block [%1]" ).arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
}

void QgsDwgImporter::setBlock( const int handle )
{
  Q_UNUSED( handle );
  QgsDebugCall;
}

void QgsDwgImporter::endBlock()
{
  QgsDebugCall;
  QgsDebugMsg( QString( "block 0x%1 ended" ).arg( mBlockHandle, 0, 16 ) );
  mBlockHandle = -1;
}

void QgsDwgImporter::addEntity( OGRFeatureDefnH dfn, OGRFeatureH f, const DRW_Entity &data )
{
  QgsDebugCall;

  QgsDebugMsg( QString( "handle:0x%1 block:0x%2" ).arg( data.handle, 0, 16 ).arg( mBlockHandle, 0, 16 ) );
  SETINTEGER( handle );
  setInteger( dfn, f, "block", mBlockHandle );
  SETINTEGER( eType );
  SETINTEGER( space );
  SETSTRING( layer );
  SETSTRING( lineType );
  setInteger( dfn, f, "ocolor", data.color );
  SETINTEGER( color24 );
  SETINTEGER( transparency );
  setString( dfn, f, "color", colorString( data.color, data.color24, data.transparency, data.layer ).toUtf8().constData() );
  setInteger( dfn, f, "lweight", DRW_LW_Conv::lineWidth2dxfInt( data.lWeight ) );
  setDouble( dfn, f, "linewidth", lineWidth( data.lWeight, data.layer ) );
  setInteger( dfn, f, "ltscale", data.ltypeScale );
  SETINTEGER( visible );
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

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );

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

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "lines" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );

  double half = ( data.staangle + data.endangle ) / 2.0;
  if ( data.staangle > data.endangle )
    half += M_PI;

  double a0 = data.isccw ? data.staangle : -data.staangle;
  double a1 = data.isccw ? half : -half;
  double a2 = data.isccw ? data.endangle : -data.endangle;

  QgsDebugMsg( QString( "arc handle=0x%1 radius=%2 staangle=%3 endangle=%4 isccw=%5 half=%6" )
               .arg( data.handle, 0, 16 ).arg( data.mRadius ).arg( data.staangle ).arg( data.endangle ).arg( data.isccw ).arg( half ) );

  QgsCircularStringV2 c;
  c.setPoints( QgsPointSequenceV2()
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + cos( a0 ) * data.mRadius, data.basePoint.y + sin( a0 ) * data.mRadius )
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + cos( a1 ) * data.mRadius, data.basePoint.y + sin( a1 ) * data.mRadius )
               << QgsPointV2( QgsWKBTypes::PointZ, data.basePoint.x + cos( a2 ) * data.mRadius, data.basePoint.y + sin( a2 ) * data.mRadius )
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

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "lines" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );

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

#if 0
    QgsDebugMsg( QString( "i:%1,%2/%3 width=%4 staWidth=%5 endWidth=%6 hadBulge=%7 hasBulge=%8 l=%9 <=> %10" )
                 .arg( i0 ).arg( i1 ).arg( n )
                 .arg( width ).arg( staWidth ).arg( endWidth )
                 .arg( hadBulge ).arg( hasBulge )
                 .arg( p0.asWkt() )
                 .arg( p1.asWkt() )
               );
#endif

    if ( s.size() > 0 && ( width != staWidth || width != endWidth || hadBulge != hasBulge ) )
    {
      if ( hadBulge )
      {
        QgsCircularStringV2 *c = new QgsCircularStringV2();
        c->setPoints( s );
        // QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
        cc.addCurve( c );
      }
      else
      {
        QgsLineStringV2 *c = new QgsLineStringV2();
        c->setPoints( s );
        // QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
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

        SETDOUBLE( thickness );
        SETDOUBLE( width );

        setPoint( dfn, f, "ext", data.extPoint );

        // QgsDebugMsg( QString( "write curve:%1" ).arg( cc.asWkt() ) );

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

      SETDOUBLE( thickness );

      setPoint( dfn, f, "ext", data.extPoint );

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
      // QgsDebugMsg( QString( "write poly:%1" ).arg( poly.asWkt() ) );

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

    SETDOUBLE( thickness );
    SETDOUBLE( width );

    setPoint( dfn, f, "ext", data.extPoint );

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

#if 0
    QgsDebugMsg( QString( "i:%1,%2/%3 width=%4 staWidth=%5 endWidth=%6 hadBulge=%7 hasBulge=%8 l=%9 <=> %10" )
                 .arg( i0 ).arg( i1 ).arg( n )
                 .arg( width ).arg( staWidth ).arg( endWidth )
                 .arg( hadBulge ).arg( hasBulge )
                 .arg( p0.asWkt() )
                 .arg( p1.asWkt() )
               );
#endif

    if ( s.size() > 0 && ( width != staWidth || width != endWidth || hadBulge != hasBulge ) )
    {
      if ( hadBulge )
      {
        QgsCircularStringV2 *c = new QgsCircularStringV2();
        c->setPoints( s );
        // QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
        cc.addCurve( c );
      }
      else
      {
        QgsLineStringV2 *c = new QgsLineStringV2();
        c->setPoints( s );
        // QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
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

        SETDOUBLE( thickness );
        setDouble( dfn, f, "width", width );

        setPoint( dfn, f, "ext", data.extPoint );

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

      SETDOUBLE( thickness );

      setPoint( dfn, f, "ext", data.extPoint );

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
      // QgsDebugMsg( QString( "write poly:%1" ).arg( poly.asWkt() ) );

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
      // QgsDebugMsg( QString( "add circular string:%1" ).arg( c->asWkt() ) );
      cc.addCurve( c );
    }
    else
    {
      QgsLineStringV2 *c = new QgsLineStringV2();
      c->setPoints( s );
      // QgsDebugMsg( QString( "add line string:%1" ).arg( c->asWkt() ) );
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

    SETDOUBLE( thickness );
    setDouble( dfn, f, "width", width );

    setPoint( dfn, f, "ext", data.extPoint );

    // QgsDebugMsg( QString( "write curve:%1" ).arg( cc.asWkt() ) );

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


/**
 * Generates B-Spline open knot vector with multiplicity
 * equal to the order at the ends.
 */
static std::vector<double> knot( const DRW_Spline &data, size_t num, size_t order )
{
  if ( data.knotslist.size() == num + order )
  {
    return data.knotslist;
  }
  else
  {
    std::vector<double> v( num + order, 0. );

    for ( size_t i = 0; i < num; ++i )
      v[ order + i ] = i + 1;

    for ( size_t i = num + 1; i < v.size(); ++i )
      v[ i ] = v[num];

    return v;
  }
}

static std::vector<double> knotu( const DRW_Spline &data, size_t num, size_t order )
{
  if ( data.knotslist.size() == num + order )
  {
    return data.knotslist;
  }
  else
  {
    std::vector<double> v( num + order, 0. );

    for ( size_t i = 0; i < v.size(); ++i )
      v[i] = i;

    return v;
  }
}

static std::vector<double> rbasis( int c, double t, int npts,
                                   const std::vector<double> &x,
                                   const std::vector<double> &h )
{
  int nplusc = npts + c;
  std::vector<double> temp( nplusc, 0. );

  // calculate the first order nonrational basis functions n[i]
  for ( int i = 0; i < nplusc - 1; ++i )
  {
    if ( t >= x[i] && t < x[i+1] )
      temp[i] = 1;
  }

  // calculate the higher order nonrational base functions
  for ( int k = 2; k <= c; ++k )
  {
    for ( int i = 0; i < nplusc - k; ++i )
    {
      // if the lower order basis function is zero skip the calculation
      if ( temp[i] != 0 )
        temp[i] = (( t - x[i] ) * temp[i] ) / ( x[i+k-1] - x[i] );

      // if the lower order basis function is zero skip the calculation
      if ( temp[i+1] != 0 )
        temp[i] += (( x[i+k] - t ) * temp[i+1] ) / ( x[i+k] - x[i+1] );
    }
  }

  // pick up last point
  if ( t >= x[nplusc-1] )
    temp[ npts-1 ] = 1;

  // calculate sum for denominator of rational basis functions
  double sum = 0.;
  for ( int i = 0; i < npts; ++i )
  {
    sum += temp[i] * h[i];
  }

  std::vector<double> r( npts, 0. );

  // form rational basis functions and put in r vector
  if ( sum != 0.0 )
  {
    for ( int i = 0; i < npts; i++ )
    {
      r[i] = ( temp[i] * h[i] ) / sum;
    }
  }

  return r;
}

/**
 * Generates a rational B-spline curve using a uniform open knot vector.
 */
static void rbspline( const DRW_Spline &data,
                      size_t npts, size_t k, int p1,
                      const std::vector<QgsVector> &b,
                      const std::vector<double> &h,
                      std::vector<QgsPoint> &p )
{
  int nplusc = npts + k;

  // generate the open knot vector
  std::vector<double> x( knot( data, npts, k ) );

  // calculate the points on the rational B-spline curve
  double t = 0.;

  double step = x[nplusc-1] / ( p1 - 1 );
  for ( size_t i = 0; i < p.size(); ++i, t += step )
  {
    if ( x[nplusc-1] - t < 5e-6 )
      t = x[nplusc-1];

    // generate the basis function for this value of t
    std::vector<double> nbasis( rbasis( k, t, npts, x, h ) );

    // generate a point on the curve
    for ( size_t j = 0; j < npts; j++ )
      p[i] += b[j] * nbasis[j];
  }

}

static void rbsplinu( const DRW_Spline &data,
                      size_t npts, size_t k, int p1,
                      const std::vector<QgsVector> &b,
                      const std::vector<double> &h,
                      std::vector<QgsPoint> &p )
{
  size_t const nplusc = npts + k;

  // generate the periodic knot vector
  std::vector<double> x( knotu( data, npts, k ) );

  // calculate the points on the rational B-spline curve
  double t = k - 1;
  double const step = double( npts - k + 1 ) / ( p1 - 1 );

  for ( size_t i = 0; i < p.size(); ++i, t += step )
  {
    if ( x[nplusc-1] - t < 5e-6 )
      t = x[nplusc-1];

    // generate the base function for this value of t
    std::vector<double> nbasis( rbasis( k, t, npts, x, h ) );

    // generate a point on the curve, for x, y, z
    for ( size_t j = 0; j < npts; ++j )
    {
      p[i] += b[j] * nbasis[j];
    }
  }
}

void QgsDwgImporter::addSpline( const DRW_Spline *data )
{
  QgsDebugCall;
  Q_ASSERT( data );

  if ( data->degree < 1 || data->degree > 3 )
  {
    QgsDebugMsg( QString( "%1: unknown spline degree %2" )
                 .arg( data->handle, 0, 16 )
                 .arg( data->degree ) );
    return;
  }

  QgsDebugMsg( QString( "degree: %1 ncontrol:%2 knotslist.size():%3 controllist.size():%4 fitlist.size():%5" )
               .arg( data->degree )
               .arg( data->ncontrol )
               .arg( data->knotslist.size() )
               .arg( data->controllist.size() )
               .arg( data->fitlist.size() ) );

  std::vector<QgsVector> cps;
  for ( size_t i = 0; i < data->controllist.size(); ++i )
  {
    const DRW_Coord &p = *data->controllist[i];
    cps.push_back( QgsVector( p.x, p.y ) );
  }

  if ( data->ncontrol == 0 && data->degree != 2 )
  {
    for ( std::vector<DRW_Coord *>::size_type i = 0; i < data->fitlist.size(); ++i )
    {
      const DRW_Coord &p = *data->fitlist[i];
      cps.push_back( QgsVector( p.x, p.y ) );
    }
  }

  if ( cps.size() > 0 && data->flags & 1 )
  {
    for ( int i = 0; i < data->degree; ++i )
      cps.push_back( cps[i] );
  }

  size_t npts = cps.size();
  size_t k = data->degree + 1;
  size_t p1 = mSplineSegs * npts;

  std::vector<double> h( npts + 1, 1. );
  std::vector<QgsPoint> p( p1, QgsPoint( 0., 0. ) );

  if ( data->flags & 1 )
  {
    rbsplinu( *data, npts, k, p1, cps, h, p );
  }
  else
  {
    rbspline( *data, npts, k, p1, cps, h, p );
  }

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "polylines" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, *data );

  QgsLineStringV2 l;
  QgsPointSequenceV2 ps;
  for ( size_t i = 0; i < p.size(); ++i )
    ps << QgsPointV2( p[i] );
  l.setPoints( ps );

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

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );

  SETSTRING( name );
  SETDOUBLE( xscale );
  SETDOUBLE( yscale );
  SETDOUBLE( zscale );
  SETDOUBLE( angle );
  SETINTEGER( colcount );
  SETINTEGER( rowcount );
  SETDOUBLE( colspace );
  SETDOUBLE( rowspace );

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

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );
  setString( dfn, f, "hpattern", "SOLID" );

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

  SETDOUBLE( height );
  SETSTRING( text );
  SETDOUBLE( angle );
  SETDOUBLE( widthscale );
  SETDOUBLE( oblique );
  SETSTRING( style );
  SETINTEGER( textgen );
  SETINTEGER( alignH );
  SETINTEGER( alignV );
  SETDOUBLE( thickness );
  SETDOUBLE( interlin );

  setPoint( dfn, f, "ext", data.extPoint );

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

  SETDOUBLE( height );
  SETSTRING( text );
  SETDOUBLE( angle );
  SETDOUBLE( widthscale );
  SETDOUBLE( oblique );
  SETSTRING( style );
  SETINTEGER( textgen );
  SETINTEGER( alignH );
  SETINTEGER( alignV );
  SETDOUBLE( thickness );
  setDouble( dfn, f, "interlin", -1.0 );

  setPoint( dfn, f, "ext", data.extPoint );

  QgsPointV2 p( QgsWKBTypes::PointZ, data.secPoint.x, data.secPoint.y, data.secPoint.z );

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

void QgsDwgImporter::addHatch( const DRW_Hatch *pdata )
{
  QgsDebugCall;
  Q_ASSERT( pdata != nullptr );
  const DRW_Hatch &data = *pdata;

  OGRLayerH layer = OGR_DS_GetLayerByName( mDs, "hatches" );
  Q_ASSERT( layer );
  OGRFeatureDefnH dfn = OGR_L_GetLayerDefn( layer );
  Q_ASSERT( dfn );
  OGRFeatureH f = OGR_F_Create( dfn );
  Q_ASSERT( f );

  addEntity( dfn, f, data );

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );

  SETSTRING( name );
  SETINTEGER( solid );
  SETINTEGER( associative );
  SETINTEGER( hstyle );
  SETINTEGER( hpattern );
  SETINTEGER( doubleflag );
  SETDOUBLE( angle );
  SETDOUBLE( scale );
  SETINTEGER( deflines );

  QgsCurvePolygonV2 p;

  Q_ASSERT( data.looplist.size() == data.loopsnum );

  for ( std::vector<DRW_HatchLoop *>::size_type i = 0; i < data.loopsnum; i++ )
  {
    const DRW_HatchLoop &hatchLoop = *data.looplist[i];

    QgsCompoundCurveV2 *cc = new QgsCompoundCurveV2();

    for ( std::vector<DRW_Entity *>::size_type j = 0;  j < hatchLoop.objlist.size(); j++ )
    {
      Q_ASSERT( hatchLoop.objlist[j] );
      const DRW_Entity *entity = hatchLoop.objlist[j];

      const DRW_LWPolyline *lwp = dynamic_cast<const DRW_LWPolyline *>( entity );
      const DRW_Line *l = dynamic_cast<const DRW_Line *>( entity );
      if ( lwp )
      {
        curveFromLWPolyline( *lwp, *cc );
      }
      else if ( l )
      {
        QgsLineStringV2 *ls = new QgsLineStringV2();
        ls->setPoints( QgsPointSequenceV2()
                       << QgsPointV2( QgsWKBTypes::PointZ, l->basePoint.x, l->basePoint.y, l->basePoint.z )
                       << QgsPointV2( QgsWKBTypes::PointZ, l->secPoint.x, l->secPoint.y, l->secPoint.z ) );
        // QgsDebugMsg( QString( "add linestring:%1" ).arg( ls->asWkt() ) );
        cc->addCurve( ls );
      }
      else
      {
        QgsDebugMsg( QString( "unknown obj %1.%2: %3" ).arg( i ).arg( j ).arg( typeid( *entity ).name() ) );
      }
    }

    if ( i == 0 )
    {
      // QgsDebugMsg( QString( "set exterior ring:%1" ).arg( cc->asWkt() ) );
      p.setExteriorRing( cc );
    }
    else
    {
      // QgsDebugMsg( QString( "set interior ring:%1" ).arg( cc->asWkt() ) );
      p.addInteriorRing( cc );
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

  SETDOUBLE( thickness );

  setPoint( dfn, f, "ext", data.extPoint );

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
