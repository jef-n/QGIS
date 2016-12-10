/***************************************************************************
                         qgsdwgimportdialog.cpp
                         ----------------------
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

#include "qgsdwgimportdialog.h"

#include <QSettings>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFileDialog>

#include "qgisapp.h"
#include "qgsdwgimporter.h"
#include "qgsvectorlayer.h"
#include "qgsmaplayerregistry.h"
#include "qgsfeatureiterator.h"
#include "qgslayertreeview.h"
#include "qgslayertreemodel.h"
#include "qgslayertreegroup.h"
#include "qgsrendererv2.h"
#include "qgsdatadefined.h"
#include "qgsnullsymbolrenderer.h"
#include "qgssinglesymbolrendererv2.h"
#include "qgsfillsymbollayerv2.h"
#include "qgslinesymbollayerv2.h"
#include "qgspallabeling.h"


struct CursorOverride
{
  CursorOverride()
  {
    QApplication::setOverrideCursor( Qt::BusyCursor );
  }

  ~CursorOverride()
  {
    QApplication::restoreOverrideCursor();
  }
};


QgsDwgImportDialog::QgsDwgImportDialog( QWidget *parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
  setupUi( this );

  QSettings s;
  leDatabase->setText( s.value( "/DwgImport/lastDatabase", "" ).toString() );
  leDrawing->setText( s.value( "/DwgImport/lastDrawing", "" ).toString() );

  on_pbLoadDatabase_clicked();
  updateUI();

  restoreGeometry( s.value( "/Windows/DwgImport/geometry" ).toByteArray() );
}

QgsDwgImportDialog::~QgsDwgImportDialog()
{
  QSettings s;
  s.setValue( "/DwgImport/lastDrawing", leDrawing->text() );
  s.setValue( "/DwgImport/lastDatabase", leDatabase->text() );
  s.setValue( "/Windows/DwgImport/geometry", saveGeometry() );
}

void QgsDwgImportDialog::updateUI()
{
  bool enableLoad = false;
  bool enableImport = false;

  if ( !leDatabase->text().isEmpty() )
  {
    QFileInfo fi( leDatabase->text() );
    enableImport = fi.exists() ? fi.isWritable() : QFileInfo( fi.path() ).isWritable();
    enableLoad = fi.isReadable();
  }

  enableImport &= !leDrawing->text().isEmpty() && QFileInfo( leDrawing->text() ).isReadable();

  pbLoadDatabase->setEnabled( enableLoad );
  pbImportDrawing->setEnabled( enableImport );
  buttonBox->button( QDialogButtonBox::Ok )->setEnabled( mLayers->rowCount() > 0 && !leLayerGroup->text().isEmpty() );
}

void QgsDwgImportDialog::on_pbBrowseDrawing_clicked()
{
  QString dir( leDrawing->text().isEmpty() ? QDir::homePath() : QFileInfo( leDrawing->text() ).canonicalPath() );
  QString filename = QFileDialog::getOpenFileName( nullptr, tr( "Select DWG/DXF file" ), dir, tr( "DXF/DWG files" ) + " (*.dwg *.DWG *.dxf *.DXF)" );
  if ( filename.isEmpty() )
    return;
  leDrawing->setText( filename );
  updateUI();
}

void QgsDwgImportDialog::on_pbBrowseDatabase_clicked()
{
  QString dir( leDatabase->text().isEmpty() ? QDir::homePath() : QFileInfo( leDatabase->text() ).canonicalPath() );
  QString filename = QFileDialog::getSaveFileName( nullptr, tr( "Specify SpatiaLite database" ), dir, tr( "SpatiaLite database" ) + " (*.db *.DB *.sqlite *.SQLITE)", nullptr, QFileDialog::DontConfirmOverwrite );
  if ( filename.isEmpty() )
    return;
  leDatabase->setText( filename );
  updateUI();
}

void QgsDwgImportDialog::on_leDatabase_textChanged( const QString &text )
{
  Q_UNUSED( text );
  updateUI();
}

void QgsDwgImportDialog::on_leDrawing_textChanged( const QString &text )
{
  Q_UNUSED( text );
  updateUI();
}

void QgsDwgImportDialog::on_leLayerGroup_textChanged( const QString &text )
{
  Q_UNUSED( text );
  updateUI();
}

void QgsDwgImportDialog::on_pbLoadDatabase_clicked()
{
  if ( !QFileInfo( leDatabase->text() ).exists() )
    return;

  CursorOverride cursor;

  CUSTOM_CRS_VALIDATION savedValidation( QgsCoordinateReferenceSystem::customSrsValidation() );
  QgsCoordinateReferenceSystem::setCustomSrsValidation( nullptr );

  QScopedPointer<QgsVectorLayer> l( new QgsVectorLayer( QString( "%1|layername=layers" ).arg( leDatabase->text() ), "layers", "ogr" ) );

  QgsCoordinateReferenceSystem::setCustomSrsValidation( savedValidation );

  if ( l && l->isValid() )
  {
    int idxName = l->fieldNameIndex( "name" );
    int idxColor = l->fieldNameIndex( "ocolor" );
    int idxFlags = l->fieldNameIndex( "flags" );

    QgsDebugMsg( QString( "idxName:%1 idxColor:%2 idxFlags:%3" ).arg( idxName ).arg( idxColor ).arg( idxFlags ) );

    QgsFeatureIterator fit = l->getFeatures( QgsFeatureRequest().setSubsetOfAttributes( QgsAttributeList() << idxName << idxColor << idxFlags ) );
    QgsFeature f;

    mLayers->setRowCount( 0 );

    while ( fit.nextFeature( f ) )
    {
      int row = mLayers->rowCount();
      mLayers->setRowCount( row + 1 );

      QgsDebugMsg( QString( "name:%1 color:%2 flags:%3" ).arg( f.attribute( idxName ).toString() ).arg( f.attribute( idxColor ).toInt() ).arg( f.attribute( idxFlags ).toString(), 0, 16 ) );

      QTableWidgetItem *item;
      item = new QTableWidgetItem( f.attribute( idxName ).toString() );
      item->setFlags( Qt::ItemIsUserCheckable | Qt::ItemIsEnabled );
      item->setCheckState( Qt::Checked );
      mLayers->setItem( row, 0, item );

      item = new QTableWidgetItem();
      item->setFlags( Qt::ItemIsUserCheckable | Qt::ItemIsEnabled );
      item->setCheckState(( f.attribute( idxColor ).toInt() >= 0 && ( f.attribute( idxFlags ).toInt() & 1 ) == 0 ) ? Qt::Checked : Qt::Unchecked );
      mLayers->setItem( row, 1, item );
    }

    mLayers->resizeColumnsToContents();

    buttonBox->button( QDialogButtonBox::Ok )->setEnabled( mLayers->rowCount() > 0 && !leLayerGroup->text().isEmpty() );
  }
  else
  {
    QgisApp::instance()->messageBar()->pushMessage( tr( "Could not open layer list" ), QgsMessageBar::CRITICAL, 4 );
  }
}

void QgsDwgImportDialog::on_pbImportDrawing_clicked()
{
  CursorOverride waitCursor;

  QgsDwgImporter importer( leDatabase->text() );

  if ( importer.import( leDrawing->text() ) )
  {
    QgisApp::instance()->messageBar()->pushMessage( tr( "Drawing import completed" ), QgsMessageBar::INFO, 4 );
  }
  else
  {
    QgisApp::instance()->messageBar()->pushMessage( tr( "Drawing import failed" ), QgsMessageBar::CRITICAL, 4 );
  }

  on_pbLoadDatabase_clicked();
}

QgsVectorLayer *QgsDwgImportDialog::layer( QgsLayerTreeGroup *layerGroup, QString layer, QString table )
{
  QgsVectorLayer *l = new QgsVectorLayer( QString( "%1|layername=%2" ).arg( leDatabase->text() ).arg( table ), table, "ogr", false );
  l->setCrs( QgsCoordinateReferenceSystem() );
  l->setSubsetString( QString( "layer='%1' AND space=0" ).arg( layer ) );
  QgsMapLayerRegistry::instance()->addMapLayer( l, false );
  layerGroup->addLayer( l );
  return l;
}

void QgsDwgImportDialog::on_buttonBox_accepted()
{
  CursorOverride waitCursor;

  CUSTOM_CRS_VALIDATION savedValidation( QgsCoordinateReferenceSystem::customSrsValidation() );
  QgsCoordinateReferenceSystem::setCustomSrsValidation( nullptr );

  QgsLayerTreeGroup *dwgGroup = QgisApp::instance()->layerTreeView()->layerTreeModel()->rootGroup()->addGroup( leLayerGroup->text() );
  Q_ASSERT( dwgGroup );

  for ( int i = 0; i < mLayers->rowCount(); i++ )
  {
    QTableWidgetItem *item = mLayers->item( i, 0 );
    if ( item->checkState() == Qt::Unchecked )
      continue;

    QString layerName( item->text() );

    QgsDebugMsg( QString( " %1" ).arg( layerName ) ) ;
    QgsLayerTreeGroup *layerGroup = dwgGroup->addGroup( layerName );
    Q_ASSERT( layerGroup );

    QgsVectorLayer *l;
    QgsSymbolV2 *sym;

    l = layer( layerGroup, layerName, "hatches" );
    QgsSimpleFillSymbolLayerV2 *sfl = new QgsSimpleFillSymbolLayerV2();
    sfl->setDataDefinedProperty( "color", new QgsDataDefined( true, false, "", "color" ) );
    sfl->setBorderStyle( Qt::NoPen );
    sym = new QgsFillSymbolV2();
    sym->changeSymbolLayer( 0, sfl );
    l->setRendererV2( new QgsSingleSymbolRendererV2( sym ) );

    l = layer( layerGroup, layerName, "lines" );
    QgsSimpleLineSymbolLayerV2 *sll = new QgsSimpleLineSymbolLayerV2();
    sll->setDataDefinedProperty( "color", new QgsDataDefined( true, false, "", "color" ) );
    sll->setPenJoinStyle( Qt::MiterJoin );
    sll->setDataDefinedProperty( "width", new QgsDataDefined( true, false, "", "linewidth" ) );
    sym = new QgsLineSymbolV2();
    sym->changeSymbolLayer( 0, sll );
    sym->setOutputUnit( QgsSymbolV2::MM );
    l->setRendererV2( new QgsSingleSymbolRendererV2( sym ) );

    l = layer( layerGroup, layerName, "polylines" );
    sll = new QgsSimpleLineSymbolLayerV2();
    sll->setDataDefinedProperty( "color", new QgsDataDefined( true, false, "", "color" ) );
    sll->setPenJoinStyle( Qt::MiterJoin );
    sll->setDataDefinedProperty( "width", new QgsDataDefined( true, false, "", "width" ) );
    sym = new QgsLineSymbolV2();
    sym->changeSymbolLayer( 0, sll );
    sym->setOutputUnit( QgsSymbolV2::MapUnit );
    l->setRendererV2( new QgsSingleSymbolRendererV2( sym ) );

    l = layer( layerGroup, layerName, "texts" );
    l->setRendererV2( new QgsNullSymbolRenderer() );

    QgsPalLayerSettings pls;
    pls.readFromLayer( l );

    pls.enabled = true;
    pls.drawLabels = true;
    pls.fieldName = "text";
    pls.fontSizeInMapUnits = true;
    pls.wrapChar = "\\P";
    pls.setDataDefinedProperty( QgsPalLayerSettings::Size, true, false, "", "height" );
    pls.setDataDefinedProperty( QgsPalLayerSettings::Color, true, false, "", "color" );
    pls.setDataDefinedProperty( QgsPalLayerSettings::MultiLineHeight, true, true, "CASE WHEN interlin<0 THEN 1 ELSE interlin*1.5 END", "" );
    pls.placement = QgsPalLayerSettings::OrderedPositionsAroundPoint;
    pls.setDataDefinedProperty( QgsPalLayerSettings::PositionX, true, true, "$x", "" );
    pls.setDataDefinedProperty( QgsPalLayerSettings::PositionY, true, true, "$y", "" );
    pls.setDataDefinedProperty( QgsPalLayerSettings::Hali, true, true, QString(
                                  "CASE"
                                  " WHEN etype=%1 THEN"
                                  " CASE"
                                  " WHEN alignv IN (1,4,7) THEN 'Left'"
                                  " WHEN alignv IN (2,5,6) THEN 'Center'"
                                  " ELSE 'Right'"
                                  " END"
                                  " ELSE"
                                  "  CASE"
                                  " WHEN alignh=0 THEN 'Left'"
                                  " WHEN alignh=1 THEN 'Center'"
                                  " WHEN alignh=2 THEN 'Right'"
                                  " WHEN alignh=3 THEN 'Left'"
                                  " WHEN alignh=4 THEN 'Left'"
                                  " END "
                                  " END" ).arg( DRW::MTEXT ), "" );

    pls.setDataDefinedProperty( QgsPalLayerSettings::Vali, true, true, QString(
                                  "CASE"
                                  " WHEN etype=%1 THEN"
                                  " CASE"
                                  " WHEN alignv < 4 THEN 'Top'"
                                  " WHEN alignv < 7 THEN 'Half'"
                                  " ELSE 'Bottom'"
                                  " END"
                                  " ELSE"
                                  " CASE"
                                  " WHEN alignv=0 THEN 'Base'"
                                  " WHEN alignv=1 THEN 'Bottom'"
                                  " WHEN alignv=2 THEN 'Half'"
                                  " WHEN alignv=3 THEN 'Top'"
                                  " END"
                                  " END" ).arg( DRW::MTEXT ), "" );

    pls.setDataDefinedProperty( QgsPalLayerSettings::Rotation, true, true, "angle*180.0/pi()", "" );

    pls.writeToLayer( l );

    l = layer( layerGroup, layerName, "inserts" );
    l = layer( layerGroup, layerName, "points" );

    layerGroup->setExpanded( false );
    layerGroup->setVisible( mLayers->item( i, 1 )->checkState() );
  }

  dwgGroup->setExpanded( false );

  QgsCoordinateReferenceSystem::setCustomSrsValidation( savedValidation );
}
