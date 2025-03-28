/*
 * Stellarium Satellites plugin config dialog
 *
 * Copyright (C) 2009, 2012 Matthew Gates
 * Copyright (C) 2015 Nick Fedoseev (Iridium flares)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
*/

#include <QDebug>
#include <QDateTime>
#include <QFileDialog>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTimer>
#include <QUrl>
#include <QTabWidget>
#include <QAction>
#include <QColorDialog>
#include <QMessageBox>

#include "StelApp.hpp"
#include "ui_satellitesDialog.h"
#include "SatellitesDialog.hpp"
#include "SatellitesImportDialog.hpp"
#include "SatellitesFilterDialog.hpp"
#include "SatellitesCommDialog.hpp"
#include "SatellitesListModel.hpp"
#include "SatellitesListFilterModel.hpp"
#include "Satellites.hpp"
#include "StelModuleMgr.hpp"
#include "StelObjectMgr.hpp"
#include "StelMovementMgr.hpp"
#include "StelStyle.hpp"
#include "StelGui.hpp"
#include "StelMainView.hpp"
#include "StelFileMgr.hpp"
#include "StelTranslator.hpp"
#include "StelActionMgr.hpp"
#include "StelMainView.hpp"

#if defined(ENABLE_XLSX) && (SATELLITES_PLUGIN_IRIDIUM == 1)
#include <xlsxdocument.h>
#include <xlsxcellrange.h>
using namespace QXlsx;
#endif

const QString SatellitesDialog::dash = QChar(0x2014);

SatellitesDialog::SatellitesDialog()
	: StelDialog("Satellites")
	, satelliteModified(false)
	, updateTimer(nullptr)
	, importWindow(nullptr)
	, filterWindow(nullptr)
	, commWindow(nullptr)
	, filterModel(nullptr)
	, checkStateRole(Qt::UserRole)
	, delimiter(", ")	
{
	ui = new Ui_satellitesDialog;
#if(SATELLITES_PLUGIN_IRIDIUM == 1)
	iridiumFlaresHeader.clear();
#endif
}

SatellitesDialog::~SatellitesDialog()
{
	if (updateTimer)
	{
		updateTimer->stop();
		delete updateTimer;
		updateTimer = nullptr;
	}

	if (importWindow)
	{
		delete importWindow;
		importWindow = nullptr;
	}

	if (filterWindow)
	{
		delete filterWindow;
		filterWindow = nullptr;
	}

	if (commWindow)
	{
		delete commWindow;
		commWindow = nullptr;
	}

	delete ui;
}

void SatellitesDialog::retranslate()
{
	if (dialog)
	{
		ui->retranslateUi(dialog);		
		updateSettingsPage(); // For the button; also calls updateCountdown()
		populateAboutPage();
		populateInfo();
		populateFilterMenu();
		updateSatelliteData();
#if(SATELLITES_PLUGIN_IRIDIUM == 1)
		initListIridiumFlares();
#endif
	}
}

// Initialize the dialog widgets and connect the signals/slots
void SatellitesDialog::createDialogContent()
{
	ui->setupUi(dialog);
#if(SATELLITES_PLUGIN_IRIDIUM == 0)
	ui->tabs->removeTab(ui->tabs->indexOf(ui->iridiumTab));
#endif
	ui->tabs->setCurrentIndex(0);	
	connect(ui->titleBar, &TitleBar::closeClicked, this, &StelDialog::close);
	connect(ui->titleBar, SIGNAL(movedTo(QPoint)), this, SLOT(handleMovedTo(QPoint)));
	connect(&StelApp::getInstance(), SIGNAL(languageChanged()), this, SLOT(retranslate()));
	Satellites* plugin = GETSTELMODULE(Satellites);

	// Kinetic scrolling
	kineticScrollingList << ui->satellitesList << ui->sourceList << ui->aboutTextBrowser;
	StelGui* gui= dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
	if (gui)
	{
		enableKineticScrolling(gui->getFlagUseKineticScrolling());
		connect(gui, SIGNAL(flagUseKineticScrollingChanged(bool)), this, SLOT(enableKineticScrolling(bool)));
	}

	// Remove any test from "color buttons"
	ui->satMarkerColorPickerButton->setText("");
	ui->satOrbitColorPickerButton->setText("");
	ui->satInfoColorPickerButton->setText("");

	// Settings tab / updates group
	// These controls are refreshed by updateSettingsPage(), which in
	// turn is triggered by setting any of these values. Because
	// clicked() is issued only by user input, there's no endless loop.
	connectBoolProperty(ui->internetUpdatesCheckbox, "Satellites.updatesEnabled");
	connectBoolProperty(ui->checkBoxAutoAdd,         "Satellites.autoAddEnabled");
	connectBoolProperty(ui->checkBoxAutoRemove,      "Satellites.autoRemoveEnabled");
	connectBoolProperty(ui->checkBoxAutoDisplay,     "Satellites.autoDisplayEnabled");
	connectIntProperty(ui->updateFrequencySpinBox,   "Satellites.updateFrequencyHours");
	ui->jumpToSourcesButton->setEnabled(ui->checkBoxAutoAdd);
	connect(ui->updateButton,            SIGNAL(clicked()),         this,   SLOT(updateTLEs()));
	connect(ui->jumpToSourcesButton,     SIGNAL(clicked()),         this,   SLOT(jumpToSourcesTab()));
	connect(plugin, SIGNAL(updateStateChanged(Satellites::UpdateState)), this, SLOT(showUpdateState(Satellites::UpdateState)));
	connect(plugin, SIGNAL(tleUpdateComplete(int, int, int, int)),       this, SLOT(showUpdateCompleted(int, int, int, int)));

	updateTimer = new QTimer(this);
	connect(updateTimer, SIGNAL(timeout()), this, SLOT(updateCountdown()));
	updateTimer->start(7000);

	// Settings tab / Visualisation settings group
	// Logic sub-group: Labels
	connectBoolProperty(ui->labelsCheckBox,     "Satellites.flagLabelsVisible");
	connectIntProperty(ui->fontSizeSpinBox,     "Satellites.labelFontSize");
	connect(ui->labelsCheckBox, SIGNAL(clicked(bool)), ui->fontSizeSpinBox, SLOT(setEnabled(bool)));
	ui->fontSizeSpinBox->setEnabled(ui->labelsCheckBox->isChecked());
	// Logic sub-group: Orbit lines
	connectBoolProperty(ui->orbitLinesCheckBox, "Satellites.flagOrbitLines");
	connectIntProperty(ui->orbitSegmentsSpin,   "Satellites.orbitLineSegments");
	connectIntProperty(ui->orbitFadeSpin,       "Satellites.orbitLineFadeSegments");
	connectIntProperty(ui->orbitDurationSpin,   "Satellites.orbitLineSegmentDuration");
	connectIntProperty(ui->orbitThicknessSpin,  "Satellites.orbitLineThickness");
	connect(ui->orbitLinesCheckBox, SIGNAL(clicked(bool)), this, SLOT(handleOrbitLinesGroup(bool)));
	handleOrbitLinesGroup(ui->orbitLinesCheckBox->isChecked());
	// Logic sub-group: Umbra
	connectBoolProperty(ui->umbraCheckBox,      "Satellites.flagUmbraVisible");
	connectBoolProperty(ui->umbraAtAltitude,    "Satellites.flagUmbraAtFixedAltitude");
	connectDoubleProperty(ui->umbraAltitude,    "Satellites.umbraAltitude");
	connect(ui->umbraCheckBox, SIGNAL(clicked(bool)), this, SLOT(handleUmbraGroup(bool)));
	handleUmbraGroup(ui->umbraCheckBox->isChecked());
	// Logic sub-group: Markers
	connectBoolProperty(ui->iconicCheckBox,		"Satellites.flagIconicMode");
	connectBoolProperty(ui->coloredInvisibleSatellites, "Satellites.flagColoredInvisible");
	connectBoolProperty(ui->hideInvisibleSatellites, "Satellites.flagHideInvisible");
	connect(ui->iconicCheckBox, SIGNAL(clicked(bool)), ui->hideInvisibleSatellites, SLOT(setEnabled(bool)));
	ui->hideInvisibleSatellites->setEnabled(ui->iconicCheckBox->isChecked());
	// Logic sub-group: Colors
	ui->invisibleColorButton->setup("Satellites.invisibleSatelliteColor", "Satellites/invisible_satellite_color");
	ui->transitColorButton  ->setup("Satellites.transitSatelliteColor",   "Satellites/transit_satellite_color");
	ui->umbraColor          ->setup("Satellites.umbraColor",              "Satellites/umbra_color");
	ui->penumbraColor       ->setup("Satellites.penumbraColor",           "Satellites/penumbra_color");
	// Logic sub-group: Penumbra
	connectBoolProperty(ui->penumbraCheckBox,    "Satellites.flagPenumbraVisible");
	// Logic sub-group: Visual filter / Altitude range
	connectBoolProperty(ui->altitudeCheckBox,     "Satellites.flagVFAltitude");
	connectDoubleProperty(ui->minAltitude,        "Satellites.minVFAltitude");
	connectDoubleProperty(ui->maxAltitude,        "Satellites.maxVFAltitude");
	enableMinMaxAltitude(ui->altitudeCheckBox->isChecked());
	connect(ui->altitudeCheckBox, SIGNAL(clicked(bool)), this, SLOT(enableMinMaxAltitude(bool)));
	// Logic sub-group: Visual filter / Magnitude range
	connectBoolProperty(ui->magnitudeCheckBox,    "Satellites.flagVFMagnitude");
	connectDoubleProperty(ui->minMagnitude,       "Satellites.minVFMagnitude");
	connectDoubleProperty(ui->maxMagnitude,       "Satellites.maxVFMagnitude");
	enableMinMaxMagnitude(ui->magnitudeCheckBox->isChecked());
	connect(ui->magnitudeCheckBox, SIGNAL(clicked(bool)), this, SLOT(enableMinMaxMagnitude(bool)));

	connect(ui->restoreDefaultsButton, SIGNAL(clicked()), this, SLOT(restoreDefaults()));
	connect(ui->saveSettingsButton,    SIGNAL(clicked()), this, SLOT(saveSettings()));
	updateSettingsPage();

	// Satellites tab
	filterModel = new SatellitesListFilterModel(this);
	filterModel->setSourceModel(plugin->getSatellitesListModel());
	filterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
	ui->satellitesList->setModel(filterModel);
	connect(ui->lineEditSearch, SIGNAL(textChanged(QString)), filterModel, SLOT(setFilterWildcard(QString)));

	QAction *clearAction = ui->lineEditSearch->addAction(QIcon(":/graphicGui/backspace-white.png"),
							     QLineEdit::ActionPosition::TrailingPosition);
	connect(clearAction, SIGNAL(triggered()), this, SLOT(searchSatellitesClear()));

	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	connect(selectionModel, SIGNAL(selectionChanged(QItemSelection,QItemSelection)),
		     this, SLOT(updateSatelliteData()));	
	connect(ui->satellitesList, SIGNAL(doubleClicked(QModelIndex)),
		     this, SLOT(trackSatellite(QModelIndex)));

	// Two-state input, three-state display
	setRightSideToROMode();
	connect(ui->displayedCheckbox, SIGNAL(clicked(bool)), ui->displayedCheckbox, SLOT(setChecked(bool)));
	connect(ui->orbitCheckbox,     SIGNAL(clicked(bool)), ui->orbitCheckbox,     SLOT(setChecked(bool)));
	connect(ui->userCheckBox,      SIGNAL(clicked(bool)), ui->userCheckBox,      SLOT(setChecked(bool)));

	// Because the previous signals and slots were connected first,
	// they will be executed before these.
	connect(ui->displayedCheckbox, SIGNAL(clicked()), this, SLOT(setFlags()));
	connect(ui->orbitCheckbox,     SIGNAL(clicked()), this, SLOT(setFlags()));
	connect(ui->userCheckBox,      SIGNAL(clicked()), this, SLOT(setFlags()));

	connect(ui->satMarkerColorPickerButton, SIGNAL(clicked(bool)), this, SLOT(askSatMarkerColor()));
	connect(ui->satOrbitColorPickerButton,  SIGNAL(clicked(bool)), this, SLOT(askSatOrbitColor()));
	connect(ui->satInfoColorPickerButton,   SIGNAL(clicked(bool)), this, SLOT(askSatInfoColor()));
	connect(ui->descriptionTextEdit,        SIGNAL(textChanged()), this, SLOT(descriptionTextChanged()));
	// Satellites tab / TLE group
	connectIntProperty(ui->validAgeSpinBox, "Satellites.tleEpochAgeDays");
	connect(ui->validAgeSpinBox, SIGNAL(valueChanged(int)), this, SLOT(updateFilteredSatellitesList()));

	connect(ui->groupsListWidget, SIGNAL(itemChanged(QListWidgetItem*)),
		     this, SLOT(handleGroupChanges(QListWidgetItem*)));

	connect(ui->groupFilterCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(filterListByGroup(int)));
	connect(ui->groupFilterCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(setRightSideToROMode()));

	importWindow = new SatellitesImportDialog();
	connect(ui->addSatellitesButton, SIGNAL(clicked()),            importWindow, SLOT(setVisible()));
	connect(importWindow, SIGNAL(satellitesAccepted(TleDataList)), this,         SLOT(addSatellites(TleDataList)));
	connect(ui->removeSatellitesButton, SIGNAL(clicked()),         this,         SLOT(removeSatellites()));
	connect(ui->selectAllButton, SIGNAL(clicked()),                this,         SLOT(selectFilteredSatellitesList()));

	filterWindow = new SatellitesFilterDialog();
	connect(ui->customFilterButton, SIGNAL(clicked()), filterWindow, SLOT(setVisible()));

	commWindow = new SatellitesCommDialog();
	connect(ui->commSatelliteButton, SIGNAL(clicked()), commWindow, SLOT(setVisible()));

	// Sources tab
	connect(ui->sourceList, SIGNAL(currentRowChanged(int)),			this, SLOT(updateButtonsProperties()));
	connect(ui->sourceList, SIGNAL(itemChanged(QListWidgetItem*)),		this,	SLOT(saveSourceList()));
	connect(ui->sourceList, SIGNAL(itemDoubleClicked(QListWidgetItem *)),	this,	SLOT(editSourceRow()));
	//FIXME: pressing Enter cause a call of addSourceRow() method...
	//connect(ui->sourceEdit, SIGNAL(returnPressed()),	this,	SLOT(saveEditedSource()));
	connect(ui->deleteSourceButton, SIGNAL(clicked()),	this, SLOT(deleteSourceRow()));
	connect(ui->addSourceButton, SIGNAL(clicked()),	        this, SLOT(addSourceRow()));
	connect(ui->editSourceButton, SIGNAL(clicked()),	this, SLOT(editSourceRow()));
	connect(ui->saveSourceButton, SIGNAL(clicked()),	this, SLOT(saveEditedSource()));
	connect(ui->resetSourcesButton, SIGNAL(clicked()),	this, SLOT(restoreTleSources()));
	connect(plugin, SIGNAL(satGroupVisibleChanged()),       this, SLOT(updateSatelliteAndSaveData()));
	connect(plugin, SIGNAL(settingsChanged()),              this, SLOT(toggleCheckableSources()));
	connect(plugin, SIGNAL(customFilterChanged()),          this, SLOT(updateFilteredSatellitesList()));	
	// bug #1350669 (https://bugs.launchpad.net/stellarium/+bug/1350669)
	connect(ui->sourceList, SIGNAL(currentRowChanged(int)), ui->sourceList, SLOT(repaint()));
	ui->editSourceButton->setEnabled(false);
	ui->deleteSourceButton->setEnabled(false);
	ui->saveSourceButton->setEnabled(false);
	ui->sourceEdit->setEnabled(false);

	// About tab
	populateAboutPage();
	populateInfo();
	populateFilterMenu();
	populateSourcesList();

#if(SATELLITES_PLUGIN_IRIDIUM == 1)
	initListIridiumFlares();
	ui->flaresPredictionDepthSpinBox->setValue(plugin->getIridiumFlaresPredictionDepth());
	connect(ui->flaresPredictionDepthSpinBox, SIGNAL(valueChanged(int)), plugin, SLOT(setIridiumFlaresPredictionDepth(int)));
	connect(ui->predictIridiumFlaresPushButton, SIGNAL(clicked()), this, SLOT(predictIridiumFlares()));
	connect(ui->predictedIridiumFlaresSaveButton, SIGNAL(clicked()), this, SLOT(savePredictedIridiumFlares()));
	connect(ui->iridiumFlaresTreeWidget, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(selectCurrentIridiumFlare(QModelIndex)));
#endif
}

void SatellitesDialog::enableMinMaxAltitude(bool state)
{
	ui->minAltitude->setEnabled(state);
	ui->maxAltitude->setEnabled(state);
}

void SatellitesDialog::enableMinMaxMagnitude(bool state)
{
	ui->minMagnitude->setEnabled(state);
	ui->maxMagnitude->setEnabled(state);
}

void SatellitesDialog::handleOrbitLinesGroup(bool state)
{
	ui->orbitSegmentsSpin->setEnabled(state);
	ui->orbitFadeSpin->setEnabled(state);
	ui->orbitDurationSpin->setEnabled(state);
	ui->orbitThicknessSpin->setEnabled(state);
}

void SatellitesDialog::handleUmbraGroup(bool state)
{
	ui->umbraAtAltitude->setEnabled(state);
	ui->umbraAltitude->setEnabled(state);
	ui->penumbraCheckBox->setEnabled(state);
}

void SatellitesDialog::askSatMarkerColor()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();

	if (selection.isEmpty()) return;

	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);

	QColor c = QColorDialog::getColor(buttonMarkerColor, &StelMainView::getInstance(), "");
	if (c.isValid())
	{
		Vec3f vColor(c);
		SatelliteP sat;
		// colourize all selected satellites
		for (int i = 0; i < selection.size(); i++)
		{
			const QModelIndex& index = selection.at(i);
			sat = SatellitesMgr->getById(index.data(Qt::UserRole).toString());
			sat->hintColor = vColor;
		}
		// colourize the button
		buttonMarkerColor = c;
		ui->satMarkerColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonMarkerColor.name() + "; }");
		saveSatellites();
	}
}

void SatellitesDialog::askSatOrbitColor()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();

	if (selection.isEmpty()) return;

	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);

	QColor c = QColorDialog::getColor(buttonOrbitColor, &StelMainView::getInstance(), "");
	if (c.isValid())
	{
		Vec3f vColor(c);
		SatelliteP sat;
		// colourize all selected satellites
		for (int i = 0; i < selection.size(); i++)
		{
			const QModelIndex& index = selection.at(i);
			sat = SatellitesMgr->getById(index.data(Qt::UserRole).toString());
			sat->orbitColor = vColor;
		}
		// colourize the button
		buttonOrbitColor = c;
		ui->satOrbitColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonOrbitColor.name() + "; }");
		saveSatellites();
	}
}

void SatellitesDialog::askSatInfoColor()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();

	if (selection.isEmpty()) return;

	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);

	QColor c = QColorDialog::getColor(buttonInfoColor, &StelMainView::getInstance(), "");
	if (c.isValid())
	{
		Vec3f vColor(c);
		SatelliteP sat;
		// colourize all selected satellites
		for (int i = 0; i < selection.size(); i++)
		{
			const QModelIndex& index = selection.at(i);
			sat = SatellitesMgr->getById(index.data(Qt::UserRole).toString());
			sat->infoColor = vColor;
		}
		// colourize the button
		buttonInfoColor = c;
		ui->satInfoColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonInfoColor.name() + "; }");
		saveSatellites();
	}
}

// save new description text to selected satellite(s)
void SatellitesDialog::descriptionTextChanged()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();

	if (selection.isEmpty()) return;

	QString newdesc = ui->descriptionTextEdit->toPlainText();
	SatelliteP sat;

	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);

	for (int i = 0; i < selection.size(); i++)
	{
		const QModelIndex& index = selection.at(i);
		sat = SatellitesMgr->getById(index.data(Qt::UserRole).toString());
		sat->description = newdesc;
	}
	saveSatellites();
}

void SatellitesDialog::searchSatellitesClear()
{
	ui->lineEditSearch->clear();
}

void SatellitesDialog::filterListByGroup(int index)
{
	if (index < 0)
		return;

	const QMap<QString, SatFlag> secondaryFilter = {
		{ "all",				SatNoFlags },
		{ "[displayed]",		SatDisplayed },
		{ "[userdefined]",		SatUser },
		{ "[undisplayed]",		SatNotDisplayed },
		{ "[newlyadded]",		SatNew },
		{ "[orbiterror]",		SatError },
		{ "[reentry]",			SatReentry },
		{ "[smallsize]",		SatSmallSize },
		{ "[mediumsize]",		SatMediumSize },
		{ "[largesize]",		SatLargeSize },
		{ "[LEO]",			SatLEO },
		{ "[GSO]",			SatGSO },
		{ "[MEO]",			SatMEO },
		{ "[HEO]",			SatHEO },
		{ "[HGSO]",			SatHGSO },
		{ "[polarorbit]",		SatPolarOrbit },
		{ "[equatorialorbit]",	SatEquatOrbit },
		{ "[PSSO]",			SatPSSO },
		{ "[HEarthO]",		SatHEarthO },
		{ "[outdatedTLE]",		SatOutdatedTLE },
		{ "[custom]",			SatCustomFilter },
		{ "[communication]",	SatCommunication },
		{ "[activeOS]",		SatActiveOS },
		{ "[operationalOS]",	SatOperationalOS },
		{ "[nonopOS]",		SatNonoperationalOS },
		{ "[partiallyopOS]",	SatPartiallyOperationalOS },
		{ "[standbyOS]",		SatStandbyOS },
		{ "[spareOS]",		SatSpareOS },
		{ "[extmissionOS]",	SatExtendedMissionOS },
		{ "[decayedOS]",		SatDecayedOS }
	};

	ui->customFilterButton->setEnabled(false);
	QString groupId = ui->groupFilterCombo->itemData(index).toString();
	if (groupId == "[custom]")
		ui->customFilterButton->setEnabled(true);

	if (groupId.contains("[") || groupId=="all")
		filterModel->setSecondaryFilters(QString(), secondaryFilter.value(groupId, SatNoFlags));
	else
		filterModel->setSecondaryFilters(groupId, SatNoFlags);

	if (ui->satellitesList->model()->rowCount() <= 0)
		return;

	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	QModelIndex first;
	if (selectionModel->hasSelection())
		first = selectionModel->selectedRows().first();
	else // Scroll to the top
		first = ui->satellitesList->model()->index(0, 0);
	selectionModel->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
	ui->satellitesList->scrollTo(first);
}

void SatellitesDialog::updateFilteredSatellitesList()
{
	QString groupId = ui->groupFilterCombo->currentData(Qt::UserRole).toString();
	if (groupId == "[outdatedTLE]" || groupId == "[custom]" || groupId == "[communication]" || groupId == "[reentry]")
	{
		filterListByGroup(ui->groupFilterCombo->currentIndex());
	}
}

void SatellitesDialog::selectFilteredSatellitesList()
{
	ui->satellitesList->selectionModel()->clearSelection();
	ui->satellitesList->selectAll();
}

void SatellitesDialog::updateSatelliteAndSaveData()
{
	updateSatelliteData(); // update properties of selected satellite in the GUI
	saveSatellites(); // enforcement saving properties of satellites
}

void SatellitesDialog::updateSatelliteData()
{
	setRightSideToRWMode();

	// NOTE: This was probably going to be used for editing satellites?
	satelliteModified = false;

	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();
	if (selection.isEmpty())
		return; // TODO: Clear the fields?

	enableSatelliteDataForm(false);

	// needed for colorbutton
	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);
	Vec3f mColor, oColor, iColor;

	// set default
	buttonMarkerColor = QColor(QColor::fromRgbF(0.7f, 0.7f, 0.7f));
	buttonOrbitColor = QColor(QColor::fromRgbF(0.7f, 0.7f, 0.7f));
	buttonInfoColor = QColor(QColor::fromRgbF(0.7f, 0.7f, 0.7f));

	if (selection.count() > 1)
	{
		ui->nameEdit->setText(QString());
		ui->noradNumberEdit->setText(QString());
		ui->cosparNumberEdit->setText(QString());
		ui->tleFirstLineEdit->setText(QString());
		ui->tleSecondLineEdit->setText(QString());
		ui->stdMagnitudeLineEdit->setText(QString());
		ui->rcsLineEdit->setText(QString());
		ui->perigeeLineEdit->setText(QString());
		ui->apogeeLineEdit->setText(QString());
		ui->periodLineEdit->setText(QString());
		ui->labelTleEpochData->setText(QString());

		// get color of first selected item and test against all other selections
		{
			const QModelIndex& index = selection.at(0);
			QString id = index.data(Qt::UserRole).toString();
			SatelliteP sat = SatellitesMgr->getById(id);

			mColor = sat->hintColor;
			oColor = sat->orbitColor;
			iColor = sat->infoColor;

			for (int i = 1; i < selection.size(); i++)
			{
				const QModelIndex& idx = selection.at(i);

				id = idx.data(Qt::UserRole).toString();
				sat = SatellitesMgr->getById(id);

				// test for more than one color in the selection.
				// if there are, return grey
				if (sat->hintColor != mColor || sat->orbitColor != oColor || sat->infoColor != iColor)
				{
					mColor = Vec3f(0.7f, 0.7f, 0.7f);
					oColor = Vec3f(0.7f, 0.7f, 0.7f);
					iColor = Vec3f(0.7f, 0.7f, 0.7f);
					break;
				}
			}
		}

		// get description text of first selection and test against all other selections
		{
			const QModelIndex& index = selection.at(0);
			QString descText = index.data(SatDescriptionRole).toString();

			if (!descText.isEmpty())
			{
				for (int i = 1; i < selection.size(); i++)
				{
					const QModelIndex& idx = selection.at(i);

					if (descText != idx.data(SatDescriptionRole).toString())
					{
						descText.clear();
						break;
					}
				}
			}

			ui->descriptionTextEdit->setText(descText);
		}

		emit SatellitesMgr->satSelectionChanged("");
	}
	else
	{
		QModelIndex& index = selection.first();
		QString id = index.data(Qt::UserRole).toString();

		float stdMagnitude = index.data(SatStdMagnitudeRole).toFloat();
		QString stdMagString = (stdMagnitude<99.f) ? QString::number(stdMagnitude, 'f', 2) : dash;
		float rcs = index.data(SatRCSRole).toFloat();
		QString rcsString = (rcs > 0.f) ? QString::number(rcs, 'f', 4) : dash;
		int perigee = qRound(index.data(SatPerigeeRole).toFloat());
		QString perigeeString = (perigee>0) ? QString::number(perigee) : dash;
		int apogee = qRound(index.data(SatApogeeRole).toFloat());
		QString apogeeString = (apogee>0) ? QString::number(apogee) : dash;
		float period = index.data(SatPeriodRole).toFloat();
		QString periodString = (period>0.f) ? QString::number(period, 'f', 2) : dash;
		QString cosparID = index.data(SatCosparIDRole).toString();

		ui->nameEdit->setText(index.data(Qt::DisplayRole).toString());
		ui->noradNumberEdit->setText(id);
		ui->cosparNumberEdit->setText(cosparID.isEmpty() ? dash : cosparID);
		// NOTE: Description is deliberately displayed untranslated!
		ui->descriptionTextEdit->setText(index.data(SatDescriptionRole).toString());
		ui->stdMagnitudeLineEdit->setText(stdMagString);
		ui->rcsLineEdit->setText(rcsString);
		ui->perigeeLineEdit->setText(perigeeString);
		ui->apogeeLineEdit->setText(apogeeString);
		ui->periodLineEdit->setText(periodString);
		ui->tleFirstLineEdit->setText(index.data(FirstLineRole).toString());
		ui->tleFirstLineEdit->setCursorPosition(0);
		ui->tleSecondLineEdit->setText(index.data(SecondLineRole).toString());
		ui->tleSecondLineEdit->setCursorPosition(0);
		ui->labelTleEpochData->setText(index.data(SatTLEEpochRole).toString());

		// get color of the one selected sat
		SatelliteP sat = SatellitesMgr->getById(id);
		mColor = sat->hintColor;
		oColor = sat->orbitColor;
		iColor = sat->infoColor;

		emit SatellitesMgr->satSelectionChanged(id);
	}

	// colourize the colorpicker button
	buttonMarkerColor=mColor.toQColor(); // .setRgbF(mColor.v[0], mColor.v[1], mColor.v[2]);
	ui->satMarkerColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonMarkerColor.name() + "; }");
	buttonOrbitColor=oColor.toQColor(); // .setRgbF(oColor.v[0], oColor.v[1], oColor.v[2]);
	ui->satOrbitColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonOrbitColor.name() + "; }");
	buttonInfoColor=iColor.toQColor(); // .setRgbF(iColor.v[0], iColor.v[1], iColor.v[2]);
	ui->satInfoColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonInfoColor.name() + "; }");

	// bug #1350669 (https://bugs.launchpad.net/stellarium/+bug/1350669)
	ui->satellitesList->repaint();

	// Things that are cumulative in a multi-selection
	GroupSet globalGroups = GETSTELMODULE(Satellites)->getGroups();
	GroupSet groupsUsedBySome;
	GroupSet groupsUsedByAll = globalGroups;
	ui->displayedCheckbox->setChecked(false);
	ui->orbitCheckbox->setChecked(false);
	ui->userCheckBox->setChecked(false);

	for (int i = 0; i < selection.size(); i++)
	{
		const QModelIndex& index = selection.at(i);

		// "Displayed" checkbox
		SatFlags flags = index.data(SatFlagsRole).value<SatFlags>();
		if (flags.testFlag(SatDisplayed))
		{
			if (!ui->displayedCheckbox->isChecked())
			{
				if (i == 0)
					ui->displayedCheckbox->setChecked(true);
				else
					ui->displayedCheckbox->setCheckState(Qt::PartiallyChecked);
			}
		}
		else
			if (ui->displayedCheckbox->isChecked())
				ui->displayedCheckbox->setCheckState(Qt::PartiallyChecked);

		// "Orbit" box
		if (flags.testFlag(SatOrbit))
		{
			if (!ui->orbitCheckbox->isChecked())
			{
				if (i == 0)
					ui->orbitCheckbox->setChecked(true);
				else
					ui->orbitCheckbox->setCheckState(Qt::PartiallyChecked);
			}
		}
		else
			if (ui->orbitCheckbox->isChecked())
				ui->orbitCheckbox->setCheckState(Qt::PartiallyChecked);

		// User ("do not update") box
		if (flags.testFlag(SatUser))
		{
			if (!ui->userCheckBox->isChecked())
			{
				if (i == 0)
					ui->userCheckBox->setChecked(true);
				else
					ui->userCheckBox->setCheckState(Qt::PartiallyChecked);
			}
		}
		else
			if (ui->userCheckBox->isChecked())
				ui->userCheckBox->setCheckState(Qt::PartiallyChecked);


		// Accumulating groups
		GroupSet groups = index.data(SatGroupsRole).value<GroupSet>();
		groupsUsedBySome.unite(groups);
		groupsUsedByAll.intersect(groups);
	}

	// Repopulate the group selector
	// Nice list of checkable, translated groups that allows adding new groups
	ui->groupsListWidget->blockSignals(true);
	ui->groupsListWidget->clear();
	for (const auto& group : std::as_const(globalGroups))
	{
		QListWidgetItem* item = new QListWidgetItem(q_(group),
							    ui->groupsListWidget);
		item->setToolTip(q_(group));
		item->setData(Qt::UserRole, group);
		Qt::CheckState state = Qt::Unchecked;
		if (groupsUsedByAll.contains(group))
			state = Qt::Checked;
		else if (groupsUsedBySome.contains(group))
			state = Qt::PartiallyChecked;
		item->setData(Qt::CheckStateRole, state);
	}
	ui->groupsListWidget->sortItems();
	addSpecialGroupItem(); // Add the "Add new..." line
	ui->groupsListWidget->blockSignals(false);

	enableSatelliteDataForm(true);
}

void SatellitesDialog::saveSatellites(void)
{
	GETSTELMODULE(Satellites)->saveCatalog();
}

void SatellitesDialog::populateAboutPage()
{
	QString jsonFileName("<tt>satellites.json</tt>");
	QString oldJsonFileName("<tt>satellites.json.old</tt>");
	QString html = "<html><head></head><body>";
	html += "<h2>" + q_("Stellarium Satellites Plugin") + "</h2><table class='layout' width=\"90%\">";
	html += "<tr width=\"30%\"><td><strong>" + q_("Version") + "</strong></td><td>" + SATELLITES_PLUGIN_VERSION + "</td></td>";
	html += "<tr><td><strong>" + q_("License") + ":</strong></td><td>" + SATELLITES_PLUGIN_LICENSE + "</td></tr>";
	html += "<tr><td rowspan=\"2\"><strong>" + q_("Authors") + "</strong></td><td>Matthew Gates &lt;matthewg42@gmail.com&gt;</td></td>";
	html += "<tr><td>Jose Luis Canales &lt;jlcanales.gasco@gmail.com&gt;</td></tr>";
	html += "<tr><td rowspan=\"5\"><strong>" + q_("Contributors") + "</strong></td><td>Bogdan Marinov &lt;bogdan.marinov84@gmail.com&gt;</td></tr>";
	html += "<tr><td>Nick Fedoseev &lt;nick.ut2uz@gmail.com&gt;</td></tr>";
	html += "<tr><td>Alexander Wolf</td></tr>";
	html += "<tr><td>Alexander Duytschaever</td></tr>";
	html += "<tr><td>Georg Zotti</td></tr></table>";

	html += "<p>" + q_("The Satellites plugin predicts the positions of artificial satellites in Earth orbit.") + "</p>";

	html += "<h3>" + q_("Notes for users") + "</h3><p><ul>";
	html += "<li>" + q_("Satellites and their orbits are only shown when the observer is on Earth.") + "</li>";
	html += "<li>" + q_("Predicted positions are only good for a fairly short time (on the order of days, weeks or perhaps a month into the past and future). Expect high weirdness when looking at dates outside this range.") + "</li>";
	html += "<li>" + q_("Orbital elements go out of date pretty quickly (over mere weeks, sometimes days).  To get useful data out, you need to update the TLE data regularly.") + "</li>";
	// TRANSLATORS: The translated names of the button and the tab are filled in automatically. You can check the original names in Stellarium. File names are not translated.
	QString resetSettingsText = QString(q_("Clicking the \"%1\" button in the \"%2\" tab of this dialog will revert to the default %3 file.  The old file will be backed up as %4.  This can be found in the user data directory, under \"modules/Satellites/\"."))
			.arg(ui->restoreDefaultsButton->text(), ui->tabs->tabText(ui->tabs->indexOf(ui->settingsTab)), jsonFileName, oldJsonFileName);
	html += "<li>" + resetSettingsText + "</li>";
	html += "<li>" + q_("The value of perigee and apogee altitudes compute for mean Earth radius.") + "</li>";
	html += "<li>" + q_("The Satellites plugin is still under development.  Some features are incomplete, missing or buggy.") + "</li>";
	html += "</ul></p>";

	// Definitions are obtained from Roscosmos documents
	html += "<h3>" + q_("Altitude classifications for geocentric orbits") + "</h3><p><ul>";
	html += "<li>" + q_("Low Earth orbit (LEO): geocentric orbits with altitudes of apogee below 4400 km, inclination of orbits in range 0-180 degrees and eccentricity below 0.25.") + "</li>";
	html += "<li>" + q_("Medium Earth orbit (MEO): geocentric orbits with altitude of apogee at least 4400 km, inclination of orbits in range 0-180 degrees, eccentricity below 0.25 and period at least 1100 minutes.") + "</li>";
	html += "<li>" + q_("Geosynchronous orbit (GSO) and geostationary orbit (GEO) are orbits with inclination of orbits below 25 degrees, eccentricity below 0.25 and period in range 1100-2000 minutes (orbits around Earth matching Earth's sidereal rotation period). ") + "</li>";
	html += "<li>" + q_("Highly elliptical orbit (HEO): geocentric orbits with altitudes of perigee below 70000 km, inclination of orbits in range 0-180 degrees, eccentricity at least 0.25 and period below 14000 minutes.") + "</li>";
	html += "<li>" + q_("High geosynchronous orbit (HGSO): geocentric orbits above the altitude of geosynchronous orbit: inclination of orbits in range 25-180 degrees, eccentricity below 0.25 and period in range 1100-2000 minutes.") + "</li>";
	// Definition from WP: https://en.wikipedia.org/wiki/High_Earth_orbit
	html += "<li>" + q_("High Earth orbit (HEO or HEO/E): a geocentric orbit with an altitude entirely above that of a geosynchronous orbit (35786 kilometres). The orbital periods of such orbits are greater than 24 hours, therefore satellites in such orbits have an apparent retrograde motion.") + "</li>";
	html += "</ul></p>";

	html += "<h3>" + q_("Inclination classifications for geocentric orbits") + "</h3><p><ul>";
	html += "<li>" + q_("Equatorial orbit: an orbit whose inclination in reference to the equatorial plane is (or very close to) 0 degrees.") + "</li>";
	html += "<li>" + q_("Polar orbit: a satellite that passes above or nearly above both poles of the planet on each revolution. Therefore it has an inclination of (or very close to) 90 degrees.") + "</li>";
	html += "<li>" + q_("Polar sun-synchronous orbit (PSSO): A nearly polar orbit that passes the equator at the same local time on every pass. Useful for image-taking satellites because shadows will be the same on every pass. Typical Sun-synchronous orbits around Earth are about 600–800 km in altitude, with periods in the 96–100-minute range, and inclinations of around 98 degrees.") + "</li>";
	html += "</ul></p>";

	// TRANSLATORS: Title of a section in the About tab of the Satellites window
	html += "<h3>" + q_("Communication links") + "</h3>";
	html += "<p>" + q_("Many satellites having transmitters (transceivers and transponders) with many modes for telemetry and data packets. You should to know which demodulator you need to decode telemetry and packets:");
	html += "<ul>";
	html += "<li>APT &mdash; " + q_("Automatic Picture Transmission") + "</li>";
	html += "<li>LRPT &mdash; " + q_("Low Resolution Picture Transmission") + "</li>";
	html += "<li>HRPT &mdash; " + q_("High Resolution Picture Transmission") + "</li>";
	html += "<li>AHRPT &mdash; " + q_("Advanced High Resolution Picture Transmission") + "</li>";
	html += "<li>AX.25 &mdash; " + q_("Amateur Radio adaptation of X.25 packet protocol") + "</li>";
	html += "<li>CW &mdash; " + q_("Continuous Wave, Morse Code") + "</li>";
	html += "<li>AM &mdash; " + q_("Amplitude Modulation") + "</li>";
	html += "<li>FM &mdash; " + q_("Frequency Modulation") + "</li>";
	html += "<li>DUV &mdash; " + q_("Data Under Voice") + "</li>";
	html += "<li>FSK &mdash; " + q_("Frequency Shift Keying") + "</li>";
	html += "<li>GFSK &mdash; " + q_("Gaussian Frequency Shift Keying") + "</li>";
	html += "<li>GMSK &mdash; " + q_("Gaussian Minimum Shift Keying") + "</li>";
	html += "<li>AFSK &mdash; " + q_("Audio Frequency Shift Keying") + "</li>";
	html += "<li>ASK &mdash; " + q_("Amplitude-shift Keying") + "</li>";
	html += "<li>PSK &mdash; " + q_("Phase-shift Keying") + "</li>";
	html += "<li>BPSK &mdash; " + q_("Binary Phase-shift Keying") + "</li>";
	html += "<li>QPSK &mdash; " + q_("Quadrature Phase-shift Keying") + "</li>";
	html += "<li>OQPSK &mdash; " + q_("Offset Quadrature Phase-shift Keying") + "</li>";
	html += "<li>DPSK &mdash; " + q_("Differential Phase-shift Keying") + "</li>";
	html += "<li>BOC &mdash; " + q_("Binary Offset Carrier") + "</li>";
	html += "<li>MBOC &mdash; " + q_("Multiplexed Binary Offset Carrier") + "</li>";
	html += "</ul></p>";

	// TRANSLATORS: Title of a section in the About tab of the Satellites window
	html += "<h3>" + q_("TLE data updates") + "</h3>";
	html += "<p>" + q_("The Satellites plugin can automatically download TLE data from Internet sources, and by default the plugin will do this if the existing data is more than 72 hours old. ");
	html += "</p><p>" + QString(q_("If you disable Internet updates, you may update from a file on your computer.  This file must be in the same format as the Celestrak updates (see %1 for an example).").arg("<a href=\"https://celestrak.org/NORAD/elements/visual.txt\">visual.txt</a>"));
	html += "</p><p>" + q_("<b>Note:</b> if the name of a satellite in update data has anything in square brackets at the end, it will be removed before the data is used.");
	html += "</p>";

	html += "<h3>" + q_("Adding new satellites") + "</h3>";
	html += "<ol><li>" + q_("Make sure the satellite(s) you wish to add are included in one of the URLs listed in the Sources tab of the satellites configuration dialog.") + "</li>";
	html += "<li>" + q_("Go to the Satellites tab, and click the '+' button.  Select the satellite(s) you wish to add and select the 'add' button.") + "</li></ol>";

	html += "<h3>" + q_("Technical notes") + "</h3>";
	html += "<p>" + q_("Positions are calculated using the SGP4 & SDP4 methods, using NORAD TLE data as the input.") + " ";
	html +=         q_("The orbital calculation code is written by Jose Luis Canales according to the revised Spacetrack Report #3 (including Spacetrack Report #6)") + " <a href=\"https://celestrak.org/publications/AIAA/2006-6753\">[*]</a>. ";
	html +=         q_("To calculate an approximate visual magnitude of satellites we use the radar cross-section (RCS) and standard magnitudes from Mike McCants' database (with permissions); the radar cross-section (RCS) from CelesTrack database; the standard magnitudes from the database of the MMT-9 observatory (Kazan Federal University)") + " <a href=\"http://mmt.favor2.info/satellites\">[**]</a>. ";
	html +=         q_("Formula to calculate an approximate visual magnitude of satellites from the standard magnitude may be found at Mike McCants website") + " <a href=\"https://mmccants.org/tles/mccdesc.html\">[***]</a>. ";
	html +=         q_("We use a spherical shape of satellite to calculate an approximate visual magnitude from RCS values.") + " ";
	html +=         q_("For modelling Starlink magnitudes we use Anthony Mallama's formula") + " <a href=\"http://www.satobs.org/seesat/Aug-2020/0079.html\">[****]</a>.</p>";

	html += StelApp::getInstance().getModuleMgr().getStandardSupportLinksInfo("Satellites plugin");
	html += "</body></html>";

	StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
	if (gui)
		ui->aboutTextBrowser->document()->setDefaultStyleSheet(QString(gui->getStelStyle().htmlStyleSheet));

	ui->aboutTextBrowser->setHtml(html);
}

void SatellitesDialog::jumpToSourcesTab()
{
	ui->tabs->setCurrentWidget(ui->sourcesTab);
}

void SatellitesDialog::updateCountdown()
{
	QString nextUpdate = q_("Next update");
	Satellites* plugin = GETSTELMODULE(Satellites);
	const bool updatesEnabled = plugin->getUpdatesEnabled();

	if (!updatesEnabled)
		ui->nextUpdateLabel->setText(q_("Internet updates disabled"));
	else if (plugin->getUpdateState() == Satellites::Updating)
		ui->nextUpdateLabel->setText(q_("Updating now..."));
	else
	{
		int secondsToUpdate = plugin->getSecondsToUpdate();
		if (secondsToUpdate <= 60)
			ui->nextUpdateLabel->setText(QString("%1: %2").arg(nextUpdate, q_("< 1 minute")));
		else if (secondsToUpdate < 3600)
		{
			int n = (secondsToUpdate/60)+1;
			// TRANSLATORS: minutes.
			ui->nextUpdateLabel->setText(QString("%1: %2 %3").arg(nextUpdate, QString::number(n), qc_("m", "time")));
		}
		else
		{
			int n = (secondsToUpdate/3600)+1;
			// TRANSLATORS: hours.
			ui->nextUpdateLabel->setText(QString("%1: %2 %3").arg(nextUpdate, QString::number(n), qc_("h", "time")));
		}
	}
}

void SatellitesDialog::showUpdateState(Satellites::UpdateState state)
{
	if (state==Satellites::Updating)
		ui->nextUpdateLabel->setText(q_("Updating now..."));
	else if (state==Satellites::DownloadError || state==Satellites::OtherError)
	{
		ui->nextUpdateLabel->setText(q_("Update error"));
		updateTimer->start();  // make sure message is displayed for a while...
	}
}

void SatellitesDialog::showUpdateCompleted(int updated, int total, int added, int missing)
{
	Satellites* plugin = GETSTELMODULE(Satellites);
	QString message;
	if (plugin->isAutoRemoveEnabled())
		message = q_("Updated %1/%2 satellite(s); %3 added; %4 removed");
	else
		message = q_("Updated %1/%2 satellite(s); %3 added; %4 missing");
	ui->nextUpdateLabel->setText(message.arg(updated).arg(total).arg(added).arg(missing));
	// display the status for another full interval before refreshing status
	updateTimer->start();
	ui->lastUpdateDateTimeEdit->setDateTime(plugin->getLastUpdate().first);
	populateFilterMenu();
}

void SatellitesDialog::saveEditedSource()
{
	// don't update the currently selected item in the source list if the text is empty or not a valid URL.
	QString u = ui->sourceEdit->text().trimmed();
	if (u.isEmpty() || u=="")
	{
		qDebug() << "SatellitesDialog::saveEditedSource empty string - not saving";
		QMessageBox::warning(&StelMainView::getInstance(), q_("Warning!"), q_("Empty string - not saving"), QMessageBox::Ok);
		return;
	}

	if (!QUrl(u).isValid() || !u.contains("://"))
	{
		qDebug() << "SatellitesDialog::saveEditedSource invalid URL - not saving : " << u;
		QMessageBox::warning(&StelMainView::getInstance(), q_("Warning!"), q_("Invalid URL - not saving"), QMessageBox::Ok);
		return;
	}

	// Changes to item data (text or check state) are connected to
	// saveSourceList(), so there's no need to call it explicitly.
	if (ui->sourceList->currentItem()!=nullptr)
		ui->sourceList->currentItem()->setText(u);
	else if (ui->sourceList->findItems(u, Qt::MatchExactly).count() <= 0)
	{
		QListWidgetItem* i = new QListWidgetItem(u, ui->sourceList);
		i->setData(checkStateRole, Qt::Unchecked);
		i->setSelected(true);
		ui->sourceList->setCurrentItem(i);
	}
	updateButtonsProperties();
	ui->sourceEdit->setText("");
}

void SatellitesDialog::saveSourceList(void)
{
	QStringList allSources;
	for(int i=0; i<ui->sourceList->count(); i++)
	{
		QString url = ui->sourceList->item(i)->text();
		if (ui->sourceList->item(i)->data(checkStateRole) == Qt::Checked)
			url.prepend("1,");
		allSources << url;
	}
	GETSTELMODULE(Satellites)->setTleSources(allSources);
}

void SatellitesDialog::deleteSourceRow(void)
{
	if (askConfirmation())
	{
		ui->sourceEdit->setText("");
		if (ui->sourceList->currentItem())
			delete ui->sourceList->currentItem();

		updateButtonsProperties();
		saveSourceList();
	}
}

void SatellitesDialog::editSourceRow(void)
{
	ui->addSourceButton->setEnabled(false);
	ui->editSourceButton->setEnabled(false);
	ui->deleteSourceButton->setEnabled(false);
	ui->saveSourceButton->setEnabled(true);
	if (ui->sourceList->currentItem())
	{
		ui->sourceEdit->setEnabled(true);
		ui->sourceEdit->setText(ui->sourceList->currentItem()->text());
		ui->sourceEdit->selectAll();
		ui->sourceEdit->setFocus();
	}
}

void SatellitesDialog::addSourceRow(void)
{
	ui->addSourceButton->setEnabled(false);
	ui->editSourceButton->setEnabled(false);
	ui->deleteSourceButton->setEnabled(false);
	ui->saveSourceButton->setEnabled(true);
	ui->sourceEdit->setEnabled(true);
	ui->sourceEdit->setText(q_("[new source]"));
	ui->sourceEdit->selectAll();
	ui->sourceEdit->setFocus();
	ui->sourceList->blockSignals(true);
	ui->sourceList->setCurrentItem(nullptr);
	ui->sourceList->blockSignals(false);
}

void SatellitesDialog::updateButtonsProperties()
{
	ui->addSourceButton->setEnabled(true);
	ui->editSourceButton->setEnabled(true);
	ui->deleteSourceButton->setEnabled(true);
	ui->saveSourceButton->setEnabled(false);
	ui->sourceEdit->setEnabled(false);
	ui->sourceEdit->setText("");
}

void SatellitesDialog::toggleCheckableSources()
{
	QListWidget* list = ui->sourceList;
	if (list->count() < 1)
		return; // Saves effort checking it on every step

	const bool enabled = GETSTELMODULE(Satellites)->isAutoAddEnabled();
	if (!enabled == list->item(0)->data(Qt::CheckStateRole).isNull())
		return; // Nothing to do

	ui->sourceList->blockSignals(true); // Prevents saving the list...
	for (int row = 0; row < list->count(); row++)
	{
		QListWidgetItem* item = list->item(row);
		if (enabled)
		{
			item->setData(Qt::CheckStateRole, item->data(Qt::UserRole));
		}
		else
		{
			item->setData(Qt::UserRole, item->data(Qt::CheckStateRole));
			item->setData(Qt::CheckStateRole, QVariant());
		}
	}
	ui->sourceList->blockSignals(false);
	checkStateRole = enabled ? Qt::CheckStateRole : Qt::UserRole;
}

void SatellitesDialog::restoreDefaults(void)
{
	if (askConfirmation())
	{
		qDebug() << "[Satellites] restore defaults...";
		GETSTELMODULE(Satellites)->restoreDefaults();
		GETSTELMODULE(Satellites)->loadSettings();
		updateSettingsPage();
		populateFilterMenu();
		populateSourcesList();
		// handle GUI elements
		ui->fontSizeSpinBox->setEnabled(ui->labelsCheckBox->isChecked());
		handleOrbitLinesGroup(ui->orbitLinesCheckBox->isChecked());
		handleUmbraGroup(ui->umbraCheckBox->isChecked());
		ui->hideInvisibleSatellites->setEnabled(ui->iconicCheckBox->isChecked());
	}
	else
		qDebug() << "[Satellites] restore defaults is canceled...";
}

void SatellitesDialog::restoreTleSources(void)
{
	if (askConfirmation())
	{
		qDebug() << "[Satellites] restore TLE sources...";
		GETSTELMODULE(Satellites)->restoreDefaultTleSources();
		GETSTELMODULE(Satellites)->loadSettings();
		populateSourcesList();
	}
	else
		qDebug() << "[Satellites] restore TLE sources is canceled...";
}

void SatellitesDialog::updateSettingsPage()
{
	Satellites* plugin = GETSTELMODULE(Satellites);

	// Update stuff
	const bool updatesEnabled = plugin->getUpdatesEnabled();
	if(updatesEnabled)
		ui->updateButton->setText(q_("Update now"));
	else
		ui->updateButton->setText(q_("Update from files"));
	ui->lastUpdateDateTimeEdit->setDateTime(plugin->getLastUpdate().first);

	updateCountdown();
}

void SatellitesDialog::populateFilterMenu()
{
	// Save current selection, if any...
	QString selectedId;
	int index = ui->groupFilterCombo->currentIndex();
	if (ui->groupFilterCombo->count() > 0 && index >= 0)
	{
		selectedId = ui->groupFilterCombo->itemData(index).toString();
	}

	// Prevent the list from re-filtering
	ui->groupFilterCombo->blockSignals(true);

	// Populate with group names/IDs
	ui->groupFilterCombo->clear();
	for (const auto& group : GETSTELMODULE(Satellites)->getGroupIdList())
	{
		ui->groupFilterCombo->addItem(q_(group), group);
	}
	ui->groupFilterCombo->model()->sort(0);

	// Add special groups - their IDs deliberately use JSON-incompatible chars.
	ui->groupFilterCombo->insertItem(0, q_("[orbit calculation error]"), QVariant("[orbiterror]"));
	ui->groupFilterCombo->insertItem(0, q_("[atmospheric entry]"), QVariant("[reentry]"));
	ui->groupFilterCombo->insertItem(0, q_("[all newly added]"), QVariant("[newlyadded]"));
	ui->groupFilterCombo->insertItem(0, q_("[all not displayed]"), QVariant("[undisplayed]"));
	ui->groupFilterCombo->insertItem(0, q_("[all displayed]"), QVariant("[displayed]"));
	ui->groupFilterCombo->insertItem(0, q_("[all communications]"), QVariant("[communication]"));
	ui->groupFilterCombo->insertItem(0, q_("[small satellites]"), QVariant("[smallsize]"));
	ui->groupFilterCombo->insertItem(0, q_("[medium satellites]"), QVariant("[mediumsize]"));
	ui->groupFilterCombo->insertItem(0, q_("[large satellites]"), QVariant("[largesize]"));
	// TRANSLATORS: LEO = Low Earth orbit
	ui->groupFilterCombo->insertItem(0, q_("[LEO satellites]"), QVariant("[LEO]"));
	// TRANSLATORS: GEO = Geosynchronous equatorial orbit (Geostationary orbit)
	ui->groupFilterCombo->insertItem(0, q_("[GEO/GSO satellites]"), QVariant("[GSO]"));
	// TRANSLATORS: MEO = Medium Earth orbit
	ui->groupFilterCombo->insertItem(0, q_("[MEO satellites]"), QVariant("[MEO]"));
	// TRANSLATORS: HEO = Highly elliptical orbit
	ui->groupFilterCombo->insertItem(0, q_("[HEO satellites]"), QVariant("[HEO]"));
	// TRANSLATORS: HGEO = High geosynchronous orbit
	ui->groupFilterCombo->insertItem(0, q_("[HGSO satellites]"), QVariant("[HGSO]"));
	ui->groupFilterCombo->insertItem(0, q_("[polar orbit satellites]"), QVariant("[polarorbit]"));
	ui->groupFilterCombo->insertItem(0, q_("[equatorial orbit satellites]"), QVariant("[equatorialorbit]"));
	// TRANSLATORS: PSSO = Polar sun synchronous orbit
	ui->groupFilterCombo->insertItem(0, q_("[PSSO satellites]"), QVariant("[PSSO]"));
	// TRANSLATORS: HEO/E = High Earth orbit
	ui->groupFilterCombo->insertItem(0, q_("[HEO/E satellites]"), QVariant("[HEarthO]"));
	ui->groupFilterCombo->insertItem(0, q_("[outdated TLE]"), QVariant("[outdatedTLE]"));
	ui->groupFilterCombo->insertItem(0, q_("[custom filter]"), QVariant("[custom]"));
	ui->groupFilterCombo->insertItem(0, q_("[all user defined]"), QVariant("[userdefined]"));	
	// Add special groups - based on SATCAT Operational Status
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Active status does not require power or communications (e.g., geodetic satellites); Active is any satellite with an operational status of +, P, B, S, or X.
	ui->groupFilterCombo->insertItem(0, q_("[active satellites]"), QVariant("[activeOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Satellites that are fully functioning
	ui->groupFilterCombo->insertItem(0, q_("[operational satellites]"), QVariant("[operationalOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Satellites that are no longer functioning
	ui->groupFilterCombo->insertItem(0, q_("[non-operational satellites]"), QVariant("[nonopOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Satellites that are partially fulfilling primary mission or secondary mission(s)
	ui->groupFilterCombo->insertItem(0, q_("[partially operational satellites]"), QVariant("[partiallyopOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Previously operational satellite put into reserve status
	ui->groupFilterCombo->insertItem(0, q_("[backup / standby satellites]"), QVariant("[standbyOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: New satellite awaiting full activation
	ui->groupFilterCombo->insertItem(0, q_("[spare satellites]"), QVariant("[spareOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Satellites with extended mission(s)
	ui->groupFilterCombo->insertItem(0, q_("[extended mission]"), QVariant("[extmissionOS]"));
	// TRANSLATORS: Satellite group [SATCAT Operational Status]: Satellites that are decayed
	ui->groupFilterCombo->insertItem(0, q_("[decayed satellites]"), QVariant("[decayedOS]"));
	// Special group - All satellites (this item should be latest in the list)
	ui->groupFilterCombo->insertItem(0, q_("[all]"), QVariant("all"));

	// Restore current selection
	index = (!selectedId.isEmpty()) ? qMax(0, ui->groupFilterCombo->findData(selectedId)) : 0;

	ui->groupFilterCombo->setCurrentIndex(index);
	ui->groupFilterCombo->blockSignals(false);
}

void SatellitesDialog::populateInfo()
{
	QString vr = q_("Valid range");
	ui->labelRCS->setText(QString("%1, %2<sup>2</sup>:").arg(q_("RCS"), qc_("m","distance")));
	ui->labelRCS->setToolTip(QString("<p>%1</p>").arg(q_("Radar cross-section (RCS) is a measure of how detectable an object is with a radar. A larger RCS indicates that an object is more easily detected.")));
	ui->labelStdMagnitude->setToolTip(QString("<p>%1</p>").arg(q_("The standard magnitude of a satellite is defined as its apparent magnitude when at half-phase and at a distance 1000 km from the observer.")));
	// TRANSLATORS: duration in seconds
	QString s = qc_("s","time");
	ui->orbitDurationSpin->setSuffix(QString(" %1").arg(s));
	// TRANSLATORS: duration in hours
	ui->updateFrequencySpinBox->setSuffix(QString(" %1").arg(qc_("h","time")));
	// TRANSLATORS: Unit of measure for distance - kilometers
	QString km = qc_("km", "distance");
	QString px = qc_("px", "pixels");
	ui->minAltitude->setSuffix(QString(" %1").arg(km));
	ui->minAltitude->setToolTip(QString("%1: %2..%3 %4").arg(vr, QString::number(ui->minAltitude->minimum(), 'f', 0), QString::number(ui->minAltitude->maximum(), 'f', 0), km));
	ui->maxAltitude->setSuffix(QString(" %1").arg(km));
	ui->maxAltitude->setToolTip(QString("%1: %2..%3 %4").arg(vr, QString::number(ui->maxAltitude->minimum(), 'f', 0), QString::number(ui->maxAltitude->maximum(), 'f', 0), km));
	ui->altitudeCheckBox->setToolTip(QString("<p>%1</p>").arg(q_("Display satellites and their orbits within selected range of altitudes only.")));
	ui->umbraAltitude->setSuffix(QString(" %1").arg(km));
	ui->umbraAltitude->setToolTip(QString("<p>%1. %2: %3..%4 %5</p>").arg(q_("Altitude of imagined satellite"), vr, QString::number(ui->umbraAltitude->minimum(), 'f', 1), QString::number(ui->umbraAltitude->maximum(), 'f', 1), km));
	ui->orbitSegmentsSpin->setToolTip(QString("<p>%1. %2: %3..%4</p>").arg(q_("Number of segments: number of segments used to draw the line"), vr, QString::number(ui->orbitSegmentsSpin->minimum()), QString::number(ui->orbitSegmentsSpin->maximum())));
	ui->orbitDurationSpin->setToolTip(QString("<p>%1. %2: %3..%4 %5</p>").arg(q_("Segment length: duration of a single segment in seconds"), vr, QString::number(ui->orbitDurationSpin->minimum()), QString::number(ui->orbitDurationSpin->maximum()), s));
	ui->orbitFadeSpin->setToolTip(QString("<p>%1. %2: %3..%4</p>").arg(q_("Fade length: number of segments used to draw each end of the line"), vr, QString::number(ui->orbitFadeSpin->minimum()), QString::number(ui->orbitFadeSpin->maximum())));
	ui->orbitThicknessSpin->setToolTip(QString("%1: %2..%3 %4").arg(q_("Orbit line thickness"), QString::number(ui->orbitThicknessSpin->minimum()), QString::number(ui->orbitThicknessSpin->maximum()), px));
	ui->orbitThicknessSpin->setSuffix(QString(" %1").arg(px));
	ui->minMagnitude->setToolTip(QString("%1: %2..%3").arg(vr, QString::number(ui->minMagnitude->minimum(), 'f', 2), QString::number(ui->minMagnitude->maximum(), 'f', 2)));
	ui->maxMagnitude->setToolTip(QString("%1: %2..%3").arg(vr, QString::number(ui->maxMagnitude->minimum(), 'f', 2), QString::number(ui->maxMagnitude->maximum(), 'f', 2)));
}

void SatellitesDialog::populateSourcesList()
{
	ui->sourceList->blockSignals(true);
	ui->sourceList->clear();

	Satellites* plugin = GETSTELMODULE(Satellites);
	QStringList urls = plugin->getTleSources();
	checkStateRole = plugin->isAutoAddEnabled() ? Qt::CheckStateRole : Qt::UserRole;
	for (auto url : urls)
	{
		bool checked = false;
		if (url.startsWith("1,"))
		{
			checked = true;
			url.remove(0, 2);
		}
		else if (url.startsWith("0,"))
			url.remove(0, 2);
		QListWidgetItem* item = new QListWidgetItem(url, ui->sourceList);
		item->setData(checkStateRole, checked ? Qt::Checked : Qt::Unchecked);
	}
	ui->sourceList->blockSignals(false);
	// if (ui->sourceList->count() > 0) ui->sourceList->setCurrentRow(0);
}

void SatellitesDialog::addSpecialGroupItem()
{
	// TRANSLATORS: Displayed in the satellite group selection box.
	QListWidgetItem* item = new QListWidgetItem(q_("New group..."));
	item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsEditable|Qt::ItemIsSelectable);
	// Assuming this is also the font used for the list items...
	QFont font = ui->groupsListWidget->font();
	font.setItalic(true);
	item->setFont(font);
	ui->groupsListWidget->insertItem(0, item);
}

void SatellitesDialog::setGroups()
{
	QModelIndexList selection = ui->satellitesList->selectionModel()->selectedIndexes();
	if (selection.isEmpty())
		return;

	// Let's determine what to add or remove
	// (partially checked groups are not modified)
	GroupSet groupsToAdd;
	GroupSet groupsToRemove;
	for (int row = 0; row < ui->groupsListWidget->count(); row++)
	{
		QListWidgetItem* item = ui->groupsListWidget->item(row);
		if (item->flags().testFlag(Qt::ItemIsEditable))
			continue;
		if (item->checkState() == Qt::Checked)
			groupsToAdd.insert(item->data(Qt::UserRole).toString());
		else if (item->checkState() == Qt::Unchecked)
			groupsToRemove.insert(item->data(Qt::UserRole).toString());
	}
	for (int i = 0; i < selection.count(); i++)
	{
		const QModelIndex& index = selection.at(i);
		GroupSet groups = index.data(SatGroupsRole).value<GroupSet>();
		groups.subtract(groupsToRemove);
		groups.unite(groupsToAdd);
		QVariant newGroups = QVariant::fromValue<GroupSet>(groups);
		ui->satellitesList->model()->setData(index, newGroups, SatGroupsRole);
	}
	saveSatellites();
}

void SatellitesDialog::saveSettings(void)
{
	GETSTELMODULE(Satellites)->saveSettingsToConfig();
	GETSTELMODULE(Satellites)->saveCatalog();
}

void SatellitesDialog::addSatellites(const TleDataList& newSatellites)
{
	GETSTELMODULE(Satellites)->add(newSatellites);
	saveSatellites();

	// Trigger re-loading the list to display the new satellites
	int index = ui->groupFilterCombo->findData(QVariant("[newlyadded]"));
	// TODO: Unnecessary once the model can handle changes? --BM
	if (ui->groupFilterCombo->currentIndex() == index)
		filterListByGroup(index);
	else
		ui->groupFilterCombo->setCurrentIndex(index); //Triggers the same operation

	// Select the satellites that were added just now
	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	selectionModel->clearSelection();
	QModelIndex firstSelectedIndex;
	QSet<QString> newIds;
	for (const auto& sat : newSatellites)
		newIds.insert(sat.id);
	for (int row = 0; row < ui->satellitesList->model()->rowCount(); row++)
	{
		QModelIndex index = ui->satellitesList->model()->index(row, 0);
		QString id = index.data(Qt::UserRole).toString();
		if (newIds.remove(id))
		{
			selectionModel->select(index, QItemSelectionModel::Select);
			if (!firstSelectedIndex.isValid())
				firstSelectedIndex = index;
		}
	}
	if (firstSelectedIndex.isValid())
		ui->satellitesList->scrollTo(firstSelectedIndex, QAbstractItemView::PositionAtTop);
	else
		ui->satellitesList->scrollToTop();
}

void SatellitesDialog::removeSatellites()
{
	if (askConfirmation())
	{
		QStringList idList;
		QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
		QModelIndexList selectedIndexes = selectionModel->selectedRows();
		for (const auto& index : selectedIndexes)
		{
			QString id = index.data(Qt::UserRole).toString();
			idList.append(id);
		}
		if (!idList.isEmpty())
		{
			GETSTELMODULE(Satellites)->remove(idList);
			saveSatellites();
		}
	}
}

void SatellitesDialog::setFlags()
{
	QItemSelectionModel* selectionModel = ui->satellitesList->selectionModel();
	QModelIndexList selection = selectionModel->selectedIndexes();
	for (int row = 0; row < selection.count(); row++)
	{
		const QModelIndex& index = selection.at(row);
		SatFlags flags = index.data(SatFlagsRole).value<SatFlags>();

		// If a checkbox is partially checked, the respective flag is not
		// changed.
		if (ui->displayedCheckbox->isChecked())
			flags |= SatDisplayed;
		else if (ui->displayedCheckbox->checkState() == Qt::Unchecked)
			flags &= ~SatDisplayed;

		if (ui->orbitCheckbox->isChecked())
			flags |= SatOrbit;
		else if (ui->orbitCheckbox->checkState() == Qt::Unchecked)
			flags &= ~SatOrbit;

		if (ui->userCheckBox->isChecked())
			flags |= SatUser;
		else if (ui->userCheckBox->checkState() == Qt::Unchecked)
			flags &= ~SatUser;

		QVariant value = QVariant::fromValue<SatFlags>(flags);
		ui->satellitesList->model()->setData(index, value, SatFlagsRole);
	}
	saveSatellites();
}

// Right side of GUI should be read only and clean by default (for example group in left top corner is was changed at the moment)
void SatellitesDialog::setRightSideToROMode()
{
	ui->removeSatellitesButton->setEnabled(false);
	ui->commSatelliteButton->setEnabled(false);
	ui->displayedCheckbox->setEnabled(false);
	ui->displayedCheckbox->setChecked(false);
	ui->orbitCheckbox->setEnabled(false);
	ui->orbitCheckbox->setChecked(false);
	ui->userCheckBox->setEnabled(false);
	ui->userCheckBox->setChecked(false);
	ui->nameEdit->setEnabled(false);
	ui->nameEdit->setText(QString());
	ui->noradNumberEdit->setEnabled(false);
	ui->noradNumberEdit->setText(QString());
	ui->cosparNumberEdit->setEnabled(false);
	ui->cosparNumberEdit->setText(QString());
	ui->descriptionTextEdit->setEnabled(false);
	ui->descriptionTextEdit->setText(QString());
	ui->groupsListWidget->setEnabled(false);
	ui->groupsListWidget->clear();
	ui->tleFirstLineEdit->setEnabled(false);
	ui->tleFirstLineEdit->setText(QString());
	ui->tleSecondLineEdit->setEnabled(false);
	ui->tleSecondLineEdit->setText(QString());
	ui->labelTleEpochData->setText(QString());
	ui->stdMagnitudeLineEdit->setEnabled(false);
	ui->stdMagnitudeLineEdit->setText(QString());
	ui->rcsLineEdit->setEnabled(false);
	ui->rcsLineEdit->setText(QString());
	ui->perigeeLineEdit->setEnabled(false);
	ui->perigeeLineEdit->setText(QString());
	ui->apogeeLineEdit->setEnabled(false);
	ui->apogeeLineEdit->setText(QString());
	ui->periodLineEdit->setEnabled(false);
	ui->periodLineEdit->setText(QString());

	// set default
	buttonMarkerColor = QColor(QColor::fromRgbF(0.7f, 0.7f, 0.7f));
	buttonOrbitColor = QColor(QColor::fromRgbF(0.7f, 0.7f, 0.7f));
	buttonInfoColor = QColor(QColor::fromRgbF(0.7f, 0.7f, 0.7f));
	ui->satMarkerColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonMarkerColor.name() + "; }");
	ui->satOrbitColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonOrbitColor.name() + "; }");
	ui->satInfoColorPickerButton->setStyleSheet("QToolButton { background-color:" + buttonInfoColor.name() + "; }");
}

// The status of elements on right side of GUI may be changed when satellite is selected
void SatellitesDialog::setRightSideToRWMode()
{
	ui->displayedCheckbox->setEnabled(true);
	ui->orbitCheckbox->setEnabled(true);
	ui->userCheckBox->setEnabled(true);
	ui->nameEdit->setEnabled(true);
	ui->noradNumberEdit->setEnabled(true);
	ui->cosparNumberEdit->setEnabled(true);
	ui->descriptionTextEdit->setEnabled(true);
	ui->groupsListWidget->setEnabled(true);
	ui->tleFirstLineEdit->setEnabled(true);
	ui->tleSecondLineEdit->setEnabled(true);
	ui->stdMagnitudeLineEdit->setEnabled(true);
	ui->rcsLineEdit->setEnabled(true);
	ui->removeSatellitesButton->setEnabled(true);
	ui->commSatelliteButton->setEnabled(true);
	ui->perigeeLineEdit->setEnabled(true);
	ui->apogeeLineEdit->setEnabled(true);
	ui->periodLineEdit->setEnabled(true);
}

void SatellitesDialog::handleGroupChanges(QListWidgetItem* item)
{
	ui->groupsListWidget->blockSignals(true);
	Qt::ItemFlags flags = item->flags();
	if (flags.testFlag(Qt::ItemIsEditable))
	{
		// Harmonize the item with the rest...
		flags ^= Qt::ItemIsEditable;
		item->setFlags(flags | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
		item->setCheckState(Qt::Checked);
		QString groupId = item->text().trimmed();
		item->setData(Qt::UserRole, groupId);
		item->setToolTip(q_(groupId));
		QFont font = item->font();
		font.setItalic(false);
		item->setFont(font);

		// ...and add a new one in its place.
		addSpecialGroupItem();

		GETSTELMODULE(Satellites)->addGroup(groupId);
		populateFilterMenu();
	}
	ui->groupsListWidget->blockSignals(false);
	setGroups();
}

void SatellitesDialog::trackSatellite(const QModelIndex& index)
{
	Satellites* SatellitesMgr = GETSTELMODULE(Satellites);
	Q_ASSERT(SatellitesMgr);
	QString id = index.data(Qt::UserRole).toString();
	SatelliteP sat = SatellitesMgr->getById(id);
	if (sat.isNull())
		return;

	if (!sat->orbitValid)
		return;

	// Turn on Satellite rendering if it is not already on	
	if (!ui->displayedCheckbox->isChecked())
	{
		ui->displayedCheckbox->setChecked(true);
		setFlags(); // sync GUI and model
	}

	// If Satellites are not currently displayed, make them visible.
	if (!SatellitesMgr->getFlagHintsVisible())
	{
		StelAction* setHintsAction = StelApp::getInstance().getStelActionManager()->findAction("actionShow_Satellite_Hints");
		Q_ASSERT(setHintsAction);
		setHintsAction->setChecked(true);
	}

	StelObjectP obj = qSharedPointerDynamicCast<StelObject>(sat);
	StelObjectMgr& objectMgr = StelApp::getInstance().getStelObjectMgr();
	if (objectMgr.setSelectedObject(obj))
	{
		GETSTELMODULE(StelMovementMgr)->autoZoomIn();
		GETSTELMODULE(StelMovementMgr)->setFlagTracking(true);
	}
}

void SatellitesDialog::updateTLEs(void)
{
	if(GETSTELMODULE(Satellites)->getUpdatesEnabled())
	{
		GETSTELMODULE(Satellites)->updateFromOnlineSources();
	}
	else
	{
		QStringList updateFiles = QFileDialog::getOpenFileNames(&StelMainView::getInstance(),
									q_("Select TLE Update File"),
									StelFileMgr::getDesktopDir(),
									"*.*");
		GETSTELMODULE(Satellites)->updateFromFiles(updateFiles, false);
	}
}

void SatellitesDialog::enableSatelliteDataForm(bool enabled)
{
	// NOTE: I'm still not sure if this is necessary, if the right signals are used to trigger changes...--BM
	ui->displayedCheckbox->blockSignals(!enabled);
	ui->orbitCheckbox->blockSignals(!enabled);
	ui->userCheckBox->blockSignals(!enabled);
	ui->descriptionTextEdit->blockSignals(!enabled);	
}

#if(SATELLITES_PLUGIN_IRIDIUM == 1)
void SatellitesDialog::setIridiumFlaresHeaderNames()
{
	iridiumFlaresHeader.clear();

	iridiumFlaresHeader << q_("Time");
	iridiumFlaresHeader << q_("Brightness");
	iridiumFlaresHeader << q_("Altitude");
	iridiumFlaresHeader << q_("Azimuth");
	iridiumFlaresHeader << q_("Satellite");

	ui->iridiumFlaresTreeWidget->setHeaderLabels(iridiumFlaresHeader);

	// adjust the column width
	for(int i = 0; i < IridiumFlaresCount; ++i)
	{
	    ui->iridiumFlaresTreeWidget->resizeColumnToContents(i);
	}

	// sort-by-date
	ui->iridiumFlaresTreeWidget->sortItems(IridiumFlaresDate, Qt::AscendingOrder);
}

void SatellitesDialog::initListIridiumFlares()
{
	ui->iridiumFlaresTreeWidget->clear();
	ui->iridiumFlaresTreeWidget->setColumnCount(IridiumFlaresCount);
	setIridiumFlaresHeaderNames();
	ui->iridiumFlaresTreeWidget->header()->setSectionsMovable(false);
}

void SatellitesDialog::predictIridiumFlares()
{
	IridiumFlaresPredictionList predictions = GETSTELMODULE(Satellites)->getIridiumFlaresPrediction();

	ui->iridiumFlaresTreeWidget->clear();
	for (const auto& flare : predictions)
	{
		SatPIFTreeWidgetItem *treeItem = new SatPIFTreeWidgetItem(ui->iridiumFlaresTreeWidget);
		QString dt = flare.datetime;
		treeItem->setText(IridiumFlaresDate, QString("%1 %2").arg(dt.left(10)).arg(dt.right(8)));
		treeItem->setText(IridiumFlaresMagnitude, QString::number(flare.magnitude,'f',1));
		treeItem->setTextAlignment(IridiumFlaresMagnitude, Qt::AlignRight);
		treeItem->setText(IridiumFlaresAltitude, StelUtils::radToDmsStr(flare.altitude));
		treeItem->setTextAlignment(IridiumFlaresAltitude, Qt::AlignRight);
		treeItem->setText(IridiumFlaresAzimuth, StelUtils::radToDmsStr(flare.azimuth));
		treeItem->setTextAlignment(IridiumFlaresAzimuth, Qt::AlignRight);
		treeItem->setText(IridiumFlaresSatellite, flare.satellite);
	}

	for(int i = 0; i < IridiumFlaresCount; ++i)
	{
	    ui->iridiumFlaresTreeWidget->resizeColumnToContents(i);
	}
}

void SatellitesDialog::selectCurrentIridiumFlare(const QModelIndex &modelIndex)
{
	StelCore* core = StelApp::getInstance().getCore();
	// Find the object
	QString name = modelIndex.sibling(modelIndex.row(), IridiumFlaresSatellite).data().toString();
	QString date = modelIndex.sibling(modelIndex.row(), IridiumFlaresDate).data().toString();
	bool ok;
	double JD  = StelUtils::getJulianDayFromISO8601String(date.left(10) + "T" + date.right(8), &ok);
	JD -= core->getUTCOffset(JD)/24.;
	JD -= core->JD_SECOND*15; // Set start point on 15 seconds before flash (TODO: should be an option in the GUI?)

	StelObjectMgr* objectMgr = GETSTELMODULE(StelObjectMgr);
	if (objectMgr->findAndSelectI18n(name) || objectMgr->findAndSelect(name))
	{
		StelApp::getInstance().getCore()->setJD(JD);
		const QList<StelObjectP> newSelected = objectMgr->getSelectedObject();
		if (!newSelected.empty())
		{
			StelMovementMgr* mvmgr = GETSTELMODULE(StelMovementMgr);
			mvmgr->moveToObject(newSelected[0], mvmgr->getAutoMoveDuration());
			mvmgr->setFlagTracking(true);
		}
	}
}

void SatellitesDialog::savePredictedIridiumFlares()
{
	QString csv  = QString("%1 (*.csv)").arg(q_("CSV (Comma delimited)"));
	QString xlsx = QString("%1 (*.xlsx)").arg(q_("Microsoft Excel Open XML Spreadsheet"));
	QString filter, defaultExtension;

	#ifdef ENABLE_XLSX
	filter = QString("%1;;%2") .arg(xlsx, csv);
	defaultExtension = "xlsx";
	#else
	filter = csv;
	defaultExtension = "csv";
	#endif

	QString defaultFilter = QString("(*.%1)").arg(defaultExtension);
	QString dir = QString("%1/iridium_flares.%2").arg(QDir::homePath(), defaultExtension);
	QString filePath = QFileDialog::getSaveFileName(&StelMainView::getInstance(), q_("Save predicted Iridium flares as..."), dir, filter, &defaultFilter);

	int count = ui->iridiumFlaresTreeWidget->topLevelItemCount();
	int columns = iridiumFlaresHeader.size();

	if (defaultFilter.contains(".csv", Qt::CaseInsensitive))
	{
		QFile predictedIridiumFlares(filePath);
		if (!predictedIridiumFlares.open(QFile::WriteOnly | QFile::Truncate))
		{
			qWarning() << "[Satellites]: Unable to open file"
					  << QDir::toNativeSeparators(filePath);
			return;
		}

		QTextStream predictedIridiumFlaresList(&predictedIridiumFlares);
		#if (QT_VERSION>=QT_VERSION_CHECK(6,0,0))
		predictedIridiumFlaresList.setEncoding(QStringConverter::Utf8);
		#else
		predictedIridiumFlaresList.setCodec("UTF-8");
		#endif
		predictedIridiumFlaresList << iridiumFlaresHeader.join(delimiter) << StelUtils::getEndLineChar();

		for (int i = 0; i < count; i++)
		{
			int columns = iridiumFlaresHeader.size();
			for (int j=0; j<columns; j++)
			{
				predictedIridiumFlaresList << ui->iridiumFlaresTreeWidget->topLevelItem(i)->text(j);
				if (j<columns-1)
					predictedIridiumFlaresList << delimiter;
				else
					predictedIridiumFlaresList << StelUtils::getEndLineChar();
			}
		}
		predictedIridiumFlares.close();
	}
	else
	{
		#ifdef ENABLE_XLSX
		int *width;
		width = new int[columns];
		QString sData;
		int w;

		QXlsx::Document xlsx;
		xlsx.setDocumentProperty("title", q_("Predicted Iridium flares"));
		xlsx.setDocumentProperty("creator", StelUtils::getApplicationName());
		xlsx.addSheet(q_("Predicted Iridium flares"), AbstractSheet::ST_WorkSheet);

		QXlsx::Format header;
		header.setHorizontalAlignment(QXlsx::Format::AlignHCenter);
		header.setPatternBackgroundColor(Qt::yellow);
		header.setBorderStyle(QXlsx::Format::BorderThin);
		header.setBorderColor(Qt::black);
		header.setFontBold(true);
		for (int i = 0; i < columns; i++)
		{
			// Row 1: Names of columns
			sData = iridiumFlaresHeader.at(i).trimmed();
			xlsx.write(1, i + 1, sData, header);
			width[i] = sData.size();
		}

		QXlsx::Format data;
		data.setHorizontalAlignment(QXlsx::Format::AlignRight);
		for (int i = 0; i < count; i++)
		{
			for (int j = 0; j < columns; j++)
			{
				// Row 2 and next: the data
				sData = ui->iridiumFlaresTreeWidget->topLevelItem(i)->text(j).trimmed();
				xlsx.write(i + 2, j + 1, sData, data);
				w = sData.size();
				if (w > width[j])
				{
					width[j] = w;
				}
			}
		}

		for (int i = 0; i < columns; i++)
		{
			xlsx.setColumnWidth(i+1, width[i]+2);
		}

		delete[] width;
		xlsx.saveAs(filePath);
		#endif
	}
}
#endif
