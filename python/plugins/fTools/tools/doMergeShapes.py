# -*- coding: utf-8 -*-

"""
***************************************************************************
    doMergeShapes.py - merge multiple shapefile into one
     --------------------------------------
    Date                 : 30-Mar-2010
    Copyright            : (C) 2010 by Alexander Bruy
    Email                : alexander dot bruy at gmail dot com
***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************
"""

from PyQt.QtCore import QObject, QSettings, QDir, QFileInfo, QFile, QThread, QMutex
from PyQt.QtWidgets import QDialog, QDialogButtonBox, QFileDialog, QMessageBox
from qgis.core import QgsVectorFileWriter, QgsVectorLayer, QgsFields, QgsFeature, QgsGeometry

import ftools_utils

from ui_frmMergeShapes import Ui_Dialog

try:
    unicode
except:
    unicode = str


class Dialog(QDialog, Ui_Dialog):

    def __init__(self, iface):
        QDialog.__init__(self, iface.mainWindow())
        self.setupUi(self)
        self.iface = iface

        self.mergeThread = None
        self.inputFiles = None
        self.outFileName = None
        self.inEncoding = None

        self.btnOk = self.buttonBox.button(QDialogButtonBox.Ok)
        self.btnClose = self.buttonBox.button(QDialogButtonBox.Close)

        self.btnSelectDir.clicked.connect(self.inputDir)
        self.btnSelectFile.clicked.connect(self.outFile)
        self.chkListMode.stateChanged.connect(self.changeMode)
        self.leOutShape.editingFinished.connect(self.updateOutFile)

    def inputDir(self):
        settings = QSettings()
        lastDir = settings.value("/fTools/lastShapeDir", ".")
        inDir = QFileDialog.getExistingDirectory(
            self,
            self.tr("Select directory with shapefiles to merge"),
            lastDir
        )

        if not inDir:
            return

        workDir = QDir(inDir)
        workDir.setFilter(QDir.Files | QDir.NoSymLinks | QDir.NoDotAndDotDot)
        nameFilter = ["*.shp", "*.SHP"]
        workDir.setNameFilters(nameFilter)
        self.inputFiles = workDir.entryList()
        if len(self.inputFiles) == 0:
            QMessageBox.warning(
                self, self.tr("No shapefiles found"),
                self.tr("There are no shapefiles in this directory. Please select another one."))
            self.inputFiles = None
            return

        settings.setValue("/fTools/lastShapeDir", inDir)

        self.progressFiles.setRange(0, len(self.inputFiles))
        self.leInputDir.setText(inDir)

    def outFile(self):
        (self.outFileName, self.encoding) = ftools_utils.saveDialog(self)
        if self.outFileName is None or self.encoding is None:
            return
        self.leOutShape.setText(self.outFileName)

    def inputFile(self):
        (files, self.inEncoding) = ftools_utils.openDialog(self, dialogMode="ManyFiles")
        if files is None or self.inEncoding is None:
            self.inputFiles = None
            return

        self.inputFiles = []
        for f in files:
            fileName = QFileInfo(f).fileName()
            self.inputFiles.append(fileName)

        self.progressFiles.setRange(0, len(self.inputFiles))
        self.leInputDir.setText(";".join(files))

    def changeMode(self):
        if self.chkListMode.isChecked():
            self.label.setText(self.tr("Input files"))
            self.btnSelectDir.clicked.disconnect(self.inputDir)
            self.btnSelectDir.clicked.connect(self.inputFile)
            self.lblGeometry.setEnabled(False)
            self.cmbGeometry.setEnabled(False)
        else:
            self.label.setText(self.tr("Input directory"))
            self.btnSelectDir.clicked.disconnect(self.inputFile)
            self.btnSelectDir.clicked.connect(self.inputDir)
            self.lblGeometry.setEnabled(True)
            self.cmbGeometry.setEnabled(True)

    def updateOutFile(self):
        self.outFileName = self.leOutShape.text()
        settings = QSettings()
        self.outEncoding = settings.value("/UI/encoding")

    def reject(self):
        QDialog.reject(self)

    def accept(self):
        if self.inputFiles is None:
            workDir = QDir(self.leInputDir.text())
            workDir.setFilter(QDir.Files | QDir.NoSymLinks | QDir.NoDotAndDotDot)
            nameFilter = ["*.shp", "*.SHP"]
            workDir.setNameFilters(nameFilter)
            self.inputFiles = workDir.entryList()
            if len(self.inputFiles) == 0:
                QMessageBox.warning(
                    self, self.tr("No shapefiles found"),
                    self.tr("There are no shapefiles in this directory. Please select another one."))
                self.inputFiles = None
                return

        if self.outFileName is None:
            QMessageBox.warning(
                self, self.tr("No output file"),
                self.tr("Please specify output file."))
            return

        if self.chkListMode.isChecked():
            files = self.leInputDir.text().split(";")
            baseDir = QFileInfo(files[0]).absolutePath()
        else:
            baseDir = self.leInputDir.text()
            # look for shapes with specified geometry type
            self.inputFiles = ftools_utils.getShapesByGeometryType(baseDir, self.inputFiles, self.cmbGeometry.currentIndex())
            if self.inputFiles is None:
                QMessageBox.warning(
                    self, self.tr("No shapefiles found"),
                    self.tr("There are no shapefiles with the given geometry type. Please select an available geometry type."))
                return
            self.progressFiles.setRange(0, len(self.inputFiles))

        outFile = QFile(self.outFileName)
        if outFile.exists():
            if not QgsVectorFileWriter.deleteShapeFile(self.outFileName):
                QMessageBox.warning(self, self.tr("Delete error"), self.tr("Can't delete file %s") % (self.outFileName))
                return

        if self.inEncoding is None:
            self.inEncoding = "System"

        self.btnOk.setEnabled(False)

        self.mergeThread = ShapeMergeThread(baseDir, self.inputFiles, self.inEncoding, self.outFileName, self.encoding)
        self.mergeThread.rangeChanged.connect(self.setFeatureProgressRange)
        self.mergeThread.checkStarted.connect(self.setFeatureProgressFormat)
        self.mergeThread.checkFinished.connect(self.resetFeatureProgressFormat)
        self.mergeThread.fileNameChanged.connect(self.setShapeProgressFormat)
        self.mergeThread.featureProcessed.connect(self.featureProcessed)
        self.mergeThread.shapeProcessed.connect(self.shapeProcessed)
        self.mergeThread.processingFinished.connect(self.processingFinished)
        self.mergeThread.processingInterrupted.connect(self.processingInterrupted)

        self.btnClose.setText(self.tr("Cancel"))
        self.buttonBox.rejected.disconnect(self.reject)
        self.btnClose.clicked.connect(self.stopProcessing)

        self.mergeThread.start()

    def setFeatureProgressRange(self, maximum):
        self.progressFeatures.setRange(0, maximum)
        self.progressFeatures.setValue(0)

    def setFeatureProgressFormat(self):
        self.progressFeatures.setFormat("Checking files: %p% ")

    def resetFeatureProgressFormat(self):
        self.progressFeatures.setFormat("%p% ")

    def featureProcessed(self):
        self.progressFeatures.setValue(self.progressFeatures.value() + 1)

    def setShapeProgressFormat(self, fileName):
        self.progressFiles.setFormat("%p% " + fileName)

    def shapeProcessed(self):
        self.progressFiles.setValue(self.progressFiles.value() + 1)

    def processingFinished(self):
        self.stopProcessing()

        if self.chkAddToCanvas.isChecked():
            if not ftools_utils.addShapeToCanvas(unicode(self.outFileName)):
                QMessageBox.warning(self, self.tr("Merging"),
                                    self.tr("Error loading output shapefile:\n%s") % (unicode(self.outFileName)))

        self.restoreGui()

    def processingInterrupted(self):
        self.restoreGui()

    def stopProcessing(self):
        if self.mergeThread is not None:
            self.mergeThread.stop()
            self.mergeThread = None

    def restoreGui(self):
        self.progressFiles.setFormat("%p%")
        self.progressFeatures.setRange(0, 100)
        self.progressFeatures.setValue(0)
        self.progressFiles.setValue(0)
        self.buttonBox.rejected.connect(self.reject)
        self.btnClose.setText(self.tr("Close"))
        self.btnOk.setEnabled(True)


class ShapeMergeThread(QThread):

    def __init__(self, dir, shapes, inputEncoding, outputFileName, outputEncoding):
        QThread.__init__(self, QThread.currentThread())
        self.baseDir = dir
        self.shapes = shapes
        self.inputEncoding = inputEncoding
        self.outputFileName = outputFileName
        self.outputEncoding = outputEncoding

        self.mutex = QMutex()
        self.stopMe = 0

    def run(self):
        self.mutex.lock()
        self.stopMe = 0
        self.mutex.unlock()

        interrupted = False

        # create attribute list with uniquie fields
        # from all selected layers
        mergedFields = []
        self.rangeChanged.emit(len(self.shapes))
        self.checkStarted.emit()

        shapeIndex = 0
        fieldMap = {}
        for fileName in self.shapes:
            layerPath = QFileInfo(self.baseDir + "/" + fileName).absoluteFilePath()
            newLayer = QgsVectorLayer(layerPath, QFileInfo(layerPath).baseName(), "ogr")
            if not newLayer.isValid():
                continue

            newLayer.setProviderEncoding(self.inputEncoding)
            vprovider = newLayer.dataProvider()
            fieldMap[shapeIndex] = {}
            fieldIndex = 0
            for layerField in vprovider.fields():
                fieldFound = False
                for mergedFieldIndex, mergedField in enumerate(mergedFields):
                    if mergedField.name() == layerField.name() and mergedField.type() == layerField.type():
                        fieldFound = True
                        fieldMap[shapeIndex][fieldIndex] = mergedFieldIndex

                        if mergedField.length() < layerField.length():
                            # suit the field size to the field of this layer
                            mergedField.setLength(layerField.length())
                        break

                if not fieldFound:
                    fieldMap[shapeIndex][fieldIndex] = len(mergedFields)
                    mergedFields.append(layerField)

                fieldIndex += 1

            shapeIndex += 1
            self.featureProcessed.emit()
        self.checkFinished.emit()

        # get information about shapefiles
        layerPath = QFileInfo(self.baseDir + "/" + self.shapes[0]).absoluteFilePath()
        newLayer = QgsVectorLayer(layerPath, QFileInfo(layerPath).baseName(), "ogr")
        self.crs = newLayer.crs()
        self.geom = newLayer.wkbType()
        vprovider = newLayer.dataProvider()

        fields = QgsFields()
        for f in mergedFields:
            fields.append(f)

        writer = QgsVectorFileWriter(
            self.outputFileName, self.outputEncoding,
            fields, self.geom, self.crs)

        shapeIndex = 0
        for fileName in self.shapes:
            layerPath = QFileInfo(self.baseDir + "/" + fileName).absoluteFilePath()
            newLayer = QgsVectorLayer(layerPath, QFileInfo(layerPath).baseName(), "ogr")
            if not newLayer.isValid():
                continue
            newLayer.setProviderEncoding(self.inputEncoding)
            vprovider = newLayer.dataProvider()
            nFeat = vprovider.featureCount()
            self.rangeChanged.emit(nFeat)
            self.fileNameChanged.emit(fileName)
            inFeat = QgsFeature()
            outFeat = QgsFeature()
            inGeom = QgsGeometry()
            fit = vprovider.getFeatures()
            while fit.nextFeature(inFeat):
                mergedAttrs = [""] * len(mergedFields)

                # fill available attributes with values
                fieldIndex = 0
                for v in inFeat.attributes():
                    if shapeIndex in fieldMap and fieldIndex in fieldMap[shapeIndex]:
                        mergedAttrs[fieldMap[shapeIndex][fieldIndex]] = v
                    fieldIndex += 1

                if inFeat.geometry() is not None:
                    inGeom = QgsGeometry(inFeat.geometry())
                    outFeat.setGeometry(inGeom)
                outFeat.setAttributes(mergedAttrs)
                writer.addFeature(outFeat)
                self.featureProcessed.emit()

            self.shapeProcessed.emit()
            self.mutex.lock()
            s = self.stopMe
            self.mutex.unlock()

            if s == 1:
                interrupted = True
                break

            shapeIndex += 1

        del writer

        if not interrupted:
            self.processingFinished.emit()
        else:
            self.processingInterrupted.emit()

    def stop(self):
        self.mutex.lock()
        self.stopMe = 1
        self.mutex.unlock()

        QThread.wait(self)
