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

QgsDwgImportDialog::QgsDwgImportDialog( QWidget *parent, Qt::WindowFlags f )
    : QDialog( parent, f )
{
  setupUi( this );

  updateUI();

  QSettings s;
  leDatabase->setText( s.value( "/DwgImport/lastDatabase", "" ).toString() );
  leDrawing->setText( s.value( "/DwgImport/lastDrawing", "" ).toString() );

  restoreGeometry( s.value( "/Windows/DwgImport/geometry" ).toByteArray() );
}

void QgsDwgImportDialog::updateUI()
{
  bool enable = false;

  if ( !leDatabase->text().isEmpty() )
  {
    QFileInfo fi( leDatabase->text() );
    enable = fi.exists() ? fi.isWritable() : QFileInfo( fi.path() ).isWritable();
  }

  enable &= !leDrawing->text().isEmpty() && QFileInfo( leDrawing->text() ).isReadable();

  pbImportDrawing->setEnabled( enable );

  // buttonBox->button( QDialogButtonBox::Ok )->setEnabled( enable );
}

QgsDwgImportDialog::~QgsDwgImportDialog()
{
  QSettings s;
  s.setValue( "/DwgImport/lastDrawing", leDrawing->text() );
  s.setValue( "/DwgImport/lastDatabase", leDatabase->text() );
  s.setValue( "/Windows/DwgImport/geometry", saveGeometry() );
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


void QgsDwgImportDialog::on_pbImportDrawing_clicked()
{
  QApplication::setOverrideCursor( Qt::BusyCursor );

  QgsDwgImporter importer( leDatabase->text() );

  if ( importer.import( leDrawing->text() ) )
  {
    QgisApp::instance()->messageBar()->pushMessage( tr( "Drawing import completed" ), QgsMessageBar::INFO, 4 );
  }
  else
  {
    QgisApp::instance()->messageBar()->pushMessage( tr( "Drawing import failed" ), QgsMessageBar::CRITICAL, 4 );
  }

  QApplication::restoreOverrideCursor();
}

void QgsDwgImportDialog::on_buttonBox_accepted()
{
}
