#include "mainwindow.hpp"
#include "ui_mainwindow.h"
#include <nowide/fstream.hpp>
#include <algorithm>
#include <unordered_map>
#include <QString>
#include <QLineEdit>
#include <QClipboard>
#include <QMenu>
#include <QAction>
#include <QDialog>
#include <QRegularExpression>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QProgressDialog>
#include <QRect>
#include <QDesktopWidget>
#include <QAudioDeviceInfo>
#include <QMetaMethod>
#include <QScreen>
#include "jam_manager.hpp"
#include "song.hpp"
#include "track.hpp"
#include "instrument.hpp"
#include "bank.hpp"
#include "bank_io.hpp"
#include "file_io.hpp"
#include "file_io_error.hpp"
#include "version.hpp"
#include "gui/command/commands_qt.hpp"
#include "gui/instrument_editor/instrument_editor_fm_form.hpp"
#include "gui/instrument_editor/instrument_editor_ssg_form.hpp"
#include "gui/module_properties_dialog.hpp"
#include "gui/groove_settings_dialog.hpp"
#include "gui/configuration_dialog.hpp"
#include "gui/comment_edit_dialog.hpp"
#include "gui/wave_export_settings_dialog.hpp"
#include "gui/vgm_export_settings_dialog.hpp"
#include "gui/instrument_selection_dialog.hpp"
#include "gui/s98_export_settings_dialog.hpp"
#include "gui/configuration_handler.hpp"
#include "chips/scci/SCCIDefines.h"
#include "gui/file_history_handler.hpp"
#include "midi/midi.hpp"
#include "audio_stream_rtaudio.hpp"
#include "color_palette_handler.hpp"
#include "binary_container.hpp"
#include "enum_hash.hpp"

MainWindow::MainWindow(std::weak_ptr<Configuration> config, QString filePath, QWidget *parent) :
	QMainWindow(parent),
	ui(new Ui::MainWindow),
	config_(config),
	palette_(std::make_shared<ColorPalette>()),
	bt_(std::make_shared<BambooTracker>(config)),
	comStack_(std::make_shared<QUndoStack>(this)),
	fileHistory_(std::make_shared<FileHistory>()),
	scciDll_(std::make_unique<QLibrary>("scci")),
	instForms_(std::make_shared<InstrumentFormManager>()),
	isModifiedForNotCommand_(false),
	isEditedPattern_(true),
	isEditedOrder_(false),
	isEditedInstList_(false),
	isSelectedPO_(false),
	firstViewUpdateRequest_(false),
	effListDiag_(std::make_unique<EffectListDialog>()),
	shortcutsDiag_(std::make_unique<KeyboardShortcutListDialog>())
{
	ui->setupUi(this);

	if (config.lock()->getMainWindowX() == -1) {	// When unset
		QRect rec = geometry();
		rec.moveCenter(QGuiApplication::screens().front()->geometry().center());
		setGeometry(rec);
		config.lock()->setMainWindowX(x());
		config.lock()->setMainWindowY(y());
	}
	else {
		move(config.lock()->getMainWindowX(), config.lock()->getMainWindowY());
	}
	resize(config.lock()->getMainWindowWidth(), config.lock()->getMainWindowHeight());
	if (config.lock()->getMainWindowMaximized()) showMaximized();
	ui->actionFollow_Mode->setChecked(config.lock()->getFollowMode());
	ui->waveVisual->setVisible(config_.lock()->getShowWaveVisual());
	bt_->setFollowPlay(config.lock()->getFollowMode());
	if (config.lock()->getPatternEditorHeaderFont().empty()) {
		config.lock()->setPatternEditorHeaderFont(ui->patternEditor->getHeaderFont().toStdString());
	}
	if (config.lock()->getPatternEditorRowsFont().empty()) {
		config.lock()->setPatternEditorRowsFont(ui->patternEditor->getRowsFont().toStdString());
	}
	if (config.lock()->getOrderListHeaderFont().empty()) {
		config.lock()->setOrderListHeaderFont(ui->orderList->getHeaderFont().toStdString());
	}
	if (config.lock()->getOrderListRowsFont().empty()) {
		config.lock()->setOrderListRowsFont(ui->orderList->getRowsFont().toStdString());
	}
	ColorPaletteHandler::loadPalette(palette_);
	updateFonts();
	setMidiConfiguration();

	/* Command stack */
	QObject::connect(comStack_.get(), &QUndoStack::indexChanged,
					 this, [&](int idx) {
		setWindowModified(idx || isModifiedForNotCommand_);
		ui->actionUndo->setEnabled(comStack_->canUndo());
		ui->actionRedo->setEnabled(comStack_->canRedo());
	});

	/* File history */
	FileHistoryHandler::loadFileHistory(fileHistory_);
	for (size_t i = 0; i < fileHistory_->size(); ++i) {
		// Leave Before Qt5.7.0 style due to windows xp
		QAction* action = ui->menu_Recent_Files->addAction(QString("&%1 %2").arg(i + 1).arg(fileHistory_->at(i)));
		action->setData(fileHistory_->at(i));
	}
	QObject::connect(ui->menu_Recent_Files, &QMenu::triggered, this, [&](QAction* action) {
		if (action != ui->actionClear) {
			if (isWindowModified()) {
				auto modTitleStd = bt_->getModuleTitle();
				QString modTitle = QString::fromUtf8(modTitleStd.c_str(), static_cast<int>(modTitleStd.length()));
				if (modTitle.isEmpty()) modTitle = tr("Untitled");
				QMessageBox dialog(QMessageBox::Warning,
								   "BambooTracker",
								   tr("Save changes to %1?").arg(modTitle),
								   QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
				switch (dialog.exec()) {
				case QMessageBox::Yes:
					if (!on_actionSave_triggered()) return;
					break;
				case QMessageBox::No:
					break;
				case QMessageBox::Cancel:
					return;
				default:
					break;
				}
			}
			openModule(action->data().toString());
		}
	});

	/* Sub tool bar */
	auto octLab = new QLabel(tr("Octave"));
	octLab->setMargin(6);
	ui->subToolBar->addWidget(octLab);
	octave_ = new QSpinBox();
	octave_->setMinimum(0);
	octave_->setMaximum(7);
	octave_->setValue(bt_->getCurrentOctave());
	auto octFunc = [&](int octave) { bt_->setCurrentOctave(octave); };
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(octave_, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, octFunc);
	ui->subToolBar->addWidget(octave_);
	ui->subToolBar->addSeparator();
	ui->subToolBar->addAction(ui->actionFollow_Mode);
	ui->subToolBar->addSeparator();
	auto hlLab1 = new QLabel(tr("Step highlight 1st"));
	hlLab1->setMargin(6);
	ui->subToolBar->addWidget(hlLab1);
	highlight1_ = new QSpinBox();
	highlight1_->setMinimum(1);
	highlight1_->setMaximum(256);
	highlight1_->setValue(8);
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(highlight1_, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int count) {
		bt_->setModuleStepHighlight1Distance(static_cast<size_t>(count));
		ui->patternEditor->setPatternHighlight1Count(count);
	});
	ui->subToolBar->addWidget(highlight1_);
	auto hlLab2 = new QLabel(tr("2nd"));
	hlLab2->setMargin(6);
	ui->subToolBar->addWidget(hlLab2);
	highlight2_ = new QSpinBox();
	highlight2_->setMinimum(1);
	highlight2_->setMaximum(256);
	highlight2_->setValue(8);
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(highlight2_, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int count) {
		bt_->setModuleStepHighlight2Distance(static_cast<size_t>(count));
		ui->patternEditor->setPatternHighlight2Count(count);
	});
	ui->subToolBar->addWidget(highlight2_);

	/* Module settings */
	QObject::connect(ui->modTitleLineEdit, &QLineEdit::textEdited,
					 this, [&](QString str) {
		bt_->setModuleTitle(str.toUtf8().toStdString());
		setModifiedTrue();
		setWindowTitle();
	});
	QObject::connect(ui->authorLineEdit, &QLineEdit::textEdited,
					 this, [&](QString str) {
		bt_->setModuleAuthor(str.toUtf8().toStdString());
		setModifiedTrue();
	});
	QObject::connect(ui->copyrightLineEdit, &QLineEdit::textEdited,
					 this, [&](QString str) {
		bt_->setModuleCopyright(str.toUtf8().toStdString());
		setModifiedTrue();
	});

	/* Edit settings */
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(ui->editableStepSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int n) {
		ui->patternEditor->setEditableStep(n);
		config.lock()->setEditableStep(static_cast<size_t>(n));
	});
	ui->editableStepSpinBox->setValue(static_cast<int>(config.lock()->getEditableStep()));
	ui->patternEditor->setEditableStep(static_cast<int>(config.lock()->getEditableStep()));

	ui->keyRepeatCheckBox->setCheckState(config.lock()->getKeyRepetition() ? Qt::Checked : Qt::Unchecked);

	/* Song number */
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(ui->songNumSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int num) {
		freezeViews();
		if (!timer_) stream_->stop();
		bt_->setCurrentSongNumber(num);
		loadSong();
		if (!timer_) stream_->start();
	});

	/* Song settings */
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(ui->tempoSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int tempo) {
		int curSong = bt_->getCurrentSongNumber();
		if (tempo != bt_->getSongTempo(curSong)) {
			bt_->setSongTempo(curSong, tempo);
			setModifiedTrue();
		}
	});
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(ui->speedSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int speed) {
		int curSong = bt_->getCurrentSongNumber();
		if (speed != bt_->getSongSpeed(curSong)) {
			bt_->setSongSpeed(curSong, speed);
			setModifiedTrue();
		}
	});
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(ui->patternSizeSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int size) {
		bt_->setDefaultPatternSize(bt_->getCurrentSongNumber(), static_cast<size_t>(size));
		ui->patternEditor->onDefaultPatternSizeChanged();
		setModifiedTrue();
	});
	// Leave Before Qt5.7.0 style due to windows xp
	QObject::connect(ui->grooveSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
					 this, [&](int n) {
		bt_->setSongGroove(bt_->getCurrentSongNumber(), n);
		setModifiedTrue();
	});

	/* Instrument list */
	updateInstrumentListColors();
	ui->instrumentListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	// Set core data to editor when add insrument
	QObject::connect(ui->instrumentListWidget->model(), &QAbstractItemModel::rowsInserted,
					 this, &MainWindow::onInstrumentListWidgetItemAdded);
	auto instToolBar = new QToolBar();
	instToolBar->setIconSize(QSize(16, 16));
	instToolBar->addAction(ui->actionNew_Instrument);
	instToolBar->addAction(ui->actionRemove_Instrument);
	instToolBar->addAction(ui->actionClone_Instrument);
	instToolBar->addSeparator();
	instToolBar->addAction(ui->actionLoad_From_File);
	instToolBar->addAction(ui->actionSave_To_File);
	instToolBar->addSeparator();
	instToolBar->addAction(ui->actionEdit);
	instToolBar->addSeparator();
	instToolBar->addAction(ui->actionRename_Instrument);
	ui->instrumentListGroupBox->layout()->addWidget(instToolBar);
	ui->instrumentListWidget->installEventFilter(this);

	/* Pattern editor */
	ui->patternEditor->setCore(bt_);
	ui->patternEditor->setCommandStack(comStack_);
	ui->patternEditor->setConfiguration(config_.lock());
	ui->patternEditor->setColorPallete(palette_);
	ui->patternEditor->installEventFilter(this);
	QObject::connect(ui->patternEditor, &PatternEditor::currentTrackChanged,
					 ui->orderList, &OrderListEditor::setCurrentTrack);
	QObject::connect(ui->patternEditor, &PatternEditor::currentOrderChanged,
					 ui->orderList, &OrderListEditor::setCurrentOrder);
	QObject::connect(ui->patternEditor, &PatternEditor::focusIn,
					 this, &MainWindow::updateMenuByPattern);
	QObject::connect(ui->patternEditor, &PatternEditor::selected,
					 this, &MainWindow::updateMenuByPatternAndOrderSelection);
	QObject::connect(ui->patternEditor, &PatternEditor::returnPressed,
					 this, [&] {
		if (bt_->isPlaySong()) stopPlaySong();
		else startPlaySong();
	});
	QObject::connect(ui->patternEditor, &PatternEditor::instrumentEntered,
					 this, [&](int num) {
		auto list = ui->instrumentListWidget;
		if (num != -1) {
			for (int i = 0; i < list->count(); ++i) {
				if (list->item(i)->data(Qt::UserRole).toInt() == num) {
					list->setCurrentRow(i);
					return ;
				}
			}
		}
	});
	QObject::connect(ui->patternEditor, &PatternEditor::effectEntered,
					 this, [&](QString text) { statusDetail_->setText(text); });

	/* Order List */
	ui->orderList->setCore(bt_);
	ui->orderList->setCommandStack(comStack_);
	ui->orderList->setConfiguration(config_.lock());
	ui->orderList->setColorPallete(palette_);
	ui->orderList->installEventFilter(this);
	QObject::connect(ui->orderList, &OrderListEditor::currentTrackChanged,
					 ui->patternEditor, &PatternEditor::setCurrentTrack);
	QObject::connect(ui->orderList, &OrderListEditor::currentOrderChanged,
					 ui->patternEditor, &PatternEditor::setCurrentOrder);
	QObject::connect(ui->orderList, &OrderListEditor::orderEdited,
					 ui->patternEditor, &PatternEditor::onOrderListEdited);
	QObject::connect(ui->orderList, &OrderListEditor::focusIn,
					 this, &MainWindow::updateMenuByOrder);
	QObject::connect(ui->orderList, &OrderListEditor::selected,
					 this, &MainWindow::updateMenuByPatternAndOrderSelection);
	QObject::connect(ui->orderList, &OrderListEditor::returnPressed,
					 this, [&] {
		if (bt_->isPlaySong()) stopPlaySong();
		else startPlaySong();
	});

	/* Visuals */
	ui->waveVisual->setColorPalette(palette_);
	visualTimer_.reset(new QTimer);
	visualTimer_->start(40);
	QObject::connect(visualTimer_.get(), &QTimer::timeout,
					 this, &MainWindow::updateVisuals);

	/* Status bar */
	statusDetail_ = new QLabel();
	statusDetail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	statusStyle_ = new QLabel();
	statusStyle_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	statusInst_ = new QLabel();
	statusInst_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	statusOctave_ = new QLabel();
	statusOctave_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	statusIntr_ = new QLabel();
	statusIntr_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	statusMixer_ = new QLabel();
	statusMixer_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	statusBpm_ = new QLabel();
	statusBpm_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	statusPlayPos_ = new QLabel();
	statusPlayPos_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
	ui->statusBar->addWidget(statusDetail_, 4);
	ui->statusBar->addPermanentWidget(statusStyle_, 1);
	ui->statusBar->addPermanentWidget(statusInst_, 1);
	ui->statusBar->addPermanentWidget(statusOctave_, 1);
	ui->statusBar->addPermanentWidget(statusIntr_, 1);
	ui->statusBar->addPermanentWidget(statusMixer_, 1);
	ui->statusBar->addPermanentWidget(statusBpm_, 1);
	ui->statusBar->addPermanentWidget(statusPlayPos_, 1);
	statusOctave_->setText(tr("Octave: %1").arg(bt_->getCurrentOctave()));
	statusIntr_->setText(QString::number(bt_->getModuleTickFrequency()) + QString("Hz"));

	/* Clipboard */
	QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged,
					 this, [&]() {
		if (isEditedOrder_) updateMenuByOrder();
		else if (isEditedPattern_) updateMenuByPattern();
	});

	/* Audio stream */
	bool savedDeviceExists = false;
	for (QAudioDeviceInfo audioDevice : QAudioDeviceInfo::availableDevices(QAudio::AudioOutput)) {
		if (audioDevice.deviceName().toUtf8().toStdString() == config.lock()->getSoundDevice()) {
			savedDeviceExists = true;
			break;
		}
	}
	if (!savedDeviceExists) {
		QString sndDev = QAudioDeviceInfo::defaultOutputDevice().deviceName();
		config.lock()->setSoundDevice(sndDev.toUtf8().toStdString());
	}
	stream_ = std::make_shared<AudioStreamRtAudio>();
	stream_->setTickUpdateCallback(+[](void* cbPtr) -> int {
		auto bt = reinterpret_cast<BambooTracker*>(cbPtr);
		return bt->streamCountUp();
	}, bt_.get());
	stream_->setGenerateCallback(+[](int16_t* container, size_t nSamples, void* cbPtr) {
		auto bt = reinterpret_cast<BambooTracker*>(cbPtr);
		bt->getStreamSamples(container, nSamples);
	}, bt_.get());
	QObject::connect(stream_.get(), &AudioStream::streamInterrupted, this, &MainWindow::onNewTickSignaled);
	bool streamState = stream_->initialize(
						   static_cast<uint32_t>(bt_->getStreamRate()),
						   static_cast<uint32_t>(bt_->getStreamDuration()),
						   bt_->getModuleTickFrequency(),
						   QString::fromUtf8(config.lock()->getSoundAPI().c_str(),
											 static_cast<int>(config.lock()->getSoundAPI().length())),
						   QString::fromUtf8(config.lock()->getSoundDevice().c_str(),
											 static_cast<int>(config.lock()->getSoundDevice().length())));
	if (!streamState) showStreamFailedDialog();
	if (config.lock()->getUseSCCI()) {
		stream_->stop();
		timer_ = std::make_unique<Timer>();
		timer_->setInterval(1000000 / bt_->getModuleTickFrequency());
		tickEventMethod_ = metaObject()->indexOfSlot("onNewTickSignaledRealChip()");
		Q_ASSERT(tickEventMethod_ != -1);
		timer_->setFunction([&]{
			QMetaMethod method = this->metaObject()->method(this->tickEventMethod_);
			method.invoke(this, Qt::QueuedConnection);
		});

		scciDll_->load();
		if (scciDll_->isLoaded()) {
			SCCIFUNC getSoundInterfaceManager = reinterpret_cast<SCCIFUNC>(
													scciDll_->resolve("getSoundInterfaceManager"));
			bt_->useSCCI(getSoundInterfaceManager ? getSoundInterfaceManager() : nullptr);
		}
		else {
			bt_->useSCCI(nullptr);
		}

		timer_->start();
	}
	else {
		bt_->useSCCI(nullptr);
		stream_->start();
	}

	/* Load module */
	if (filePath.isEmpty()) {
		loadModule();
		setInitialSelectedInstrument();
	}
	else {
		openModule(filePath);
	}

	/* MIDI */
	midiKeyEventMethod_ = metaObject()->indexOfSlot("midiKeyEvent(uchar,uchar,uchar)");
	Q_ASSERT(midiKeyEventMethod_ != -1);
	midiProgramEventMethod_ = metaObject()->indexOfSlot("midiProgramEvent(uchar,uchar)");
	Q_ASSERT(midiProgramEventMethod_ != -1);
	MidiInterface::instance().installInputHandler(&midiThreadReceivedEvent, this);
}

MainWindow::~MainWindow()
{
	MidiInterface::instance().uninstallInputHandler(&midiThreadReceivedEvent, this);
	stream_->shutdown();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (auto fmForm = qobject_cast<InstrumentEditorFMForm*>(watched)) {
		// Change current instrument by activating FM editor
		if (event->type() == QEvent::WindowActivate) {
			int row = findRowFromInstrumentList(fmForm->getInstrumentNumber());
			ui->instrumentListWidget->setCurrentRow(row);
			return false;
		}
		else if (event->type() == QEvent::Resize) {
			config_.lock()->setInstrumentFMWindowWidth(fmForm->width());
			config_.lock()->setInstrumentFMWindowHeight(fmForm->height());
			return false;
		}
	}

	if (auto ssgForm = qobject_cast<InstrumentEditorSSGForm*>(watched)) {
		// Change current instrument by activating SSG editor
		if (event->type() == QEvent::WindowActivate) {
			int row = findRowFromInstrumentList(ssgForm->getInstrumentNumber());
			ui->instrumentListWidget->setCurrentRow(row);
			return false;
		}
		else if (event->type() == QEvent::Resize) {
			config_.lock()->setInstrumentSSGWindowWidth(ssgForm->width());
			config_.lock()->setInstrumentSSGWindowHeight(ssgForm->height());
			return false;
		}
	}

	if (watched == ui->instrumentListWidget) {
		switch (event->type()) {
		case QEvent::KeyPress:
			if (dynamic_cast<QKeyEvent*>(event)->key() == Qt::Key_Insert) addInstrument();
			break;
		case QEvent::FocusIn:
			updateMenuByInstrumentList();
			break;
		default:
			break;
		}
	}

	return false;
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
	int key = event->key();

	/* Key check */
	QString seq = QKeySequence(static_cast<int>(event->modifiers()) | key).toString();
	if (seq == QKeySequence(
				QString::fromUtf8(config_.lock()->getOctaveUpKey().c_str(),
								  static_cast<int>(config_.lock()->getOctaveUpKey().length()))).toString()) {
		changeOctave(true);
		return;
	}
	else if (seq == QKeySequence(
				 QString::fromUtf8(config_.lock()->getOctaveDownKey().c_str(),
								   static_cast<int>(config_.lock()->getOctaveDownKey().length()))).toString()) {
		changeOctave(false);
		return;
	}

	/* General keys */
	switch (key) {
	case Qt::Key_F2:
		ui->patternEditor->setFocus();
		return;
	case Qt::Key_F3:
		ui->orderList->setFocus();
		return;
	case Qt::Key_F4:
		ui->instrumentListWidget->setFocus();
		updateMenuByInstrumentList();
		return;
	default:
		if (!event->isAutoRepeat()) {
			// Musical keyboard
			Qt::Key qtKey = static_cast<Qt::Key>(key);
			try {
				bt_->jamKeyOn(getJamKeyFromLayoutMapping(qtKey));
			} catch (std::invalid_argument &) {}
		}
		break;
	}
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
	int key = event->key();

	if (!event->isAutoRepeat()) {
		// Musical keyboard
		Qt::Key qtKey = static_cast<Qt::Key> (key);
		try {
			bt_->jamKeyOff (getJamKeyFromLayoutMapping (qtKey));
		} catch (std::invalid_argument &) {}
	}
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	auto mime = event->mimeData();
	if (mime->hasUrls() && mime->urls().length() == 1) {
		switch (FileIO::judgeFileTypeFromExtension(
					QFileInfo(mime->urls().first().toLocalFile()).suffix().toStdString())) {
		case FileIO::FileType::Mod:
		case FileIO::FileType::Inst:
		case FileIO::FileType::Bank:
			event->acceptProposedAction();
			break;
		default:
			break;
		}
	}
}

void MainWindow::dropEvent(QDropEvent* event)
{
	QString file = event->mimeData()->urls().first().toLocalFile();

	switch (FileIO::judgeFileTypeFromExtension(QFileInfo(file).suffix().toStdString())) {
	case FileIO::FileType::Mod:
	{
		if (isWindowModified()) {
			auto modTitleStd = bt_->getModuleTitle();
			QString modTitle = QString::fromUtf8(modTitleStd.c_str(), static_cast<int>(modTitleStd.length()));
			if (modTitle.isEmpty()) modTitle = tr("Untitled");
			QMessageBox dialog(QMessageBox::Warning,
							   "BambooTracker",
							   tr("Save changes to %1?").arg(modTitle),
							   QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
			switch (dialog.exec()) {
			case QMessageBox::Yes:
				if (!on_actionSave_triggered()) return;
				break;
			case QMessageBox::No:
				break;
			case QMessageBox::Cancel:
				return;
			default:
				break;
			}
		}

		bt_->stopPlaySong();
		lockControls(false);

		openModule(file);
		break;
	}
	case FileIO::FileType::Inst:
	{
		funcLoadInstrument(file);
		break;
	}
	case FileIO::FileType::Bank:
	{
		funcImportInstrumentsFromBank(file);
		break;
	}
	default:
		break;
	}
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);

	if (!isMaximized()) {	// Check previous size
		config_.lock()->setMainWindowWidth(event->oldSize().width());
		config_.lock()->setMainWindowHeight(event->oldSize().height());
	}
}

void MainWindow::moveEvent(QMoveEvent* event)
{
	QWidget::moveEvent(event);

	if (!isMaximized()) {	// Check previous position
		config_.lock()->setMainWindowX(event->oldPos().x());
		config_.lock()->setMainWindowY(event->oldPos().y());
	}
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (isWindowModified()) {
		auto modTitleStd = bt_->getModuleTitle();
		QString modTitle = QString::fromUtf8(modTitleStd.c_str(), static_cast<int>(modTitleStd.length()));
		if (modTitle.isEmpty()) modTitle = tr("Untitled");
		QMessageBox dialog(QMessageBox::Warning,
						   "BambooTracker",
						   tr("Save changes to %1?").arg(modTitle),
						   QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
		switch (dialog.exec()) {
		case QMessageBox::Yes:
			if (!on_actionSave_triggered()) return;
			break;
		case QMessageBox::No:
			break;
		case QMessageBox::Cancel:
			event->ignore();
			return;
		default:
			break;
		}
	}

	if (isMaximized()) {
		config_.lock()->setMainWindowMaximized(true);
	}
	else {
		config_.lock()->setMainWindowMaximized(false);
		config_.lock()->setMainWindowWidth(width());
		config_.lock()->setMainWindowHeight(height());
		config_.lock()->setMainWindowX(x());
		config_.lock()->setMainWindowY(y());
	}
	config_.lock()->setFollowMode(bt_->isFollowPlay());

	instForms_->closeAll();

	FileHistoryHandler::saveFileHistory(fileHistory_);

	event->accept();
}

void MainWindow::freezeViews()
{
	ui->orderList->freeze();
	ui->patternEditor->freeze();
}

void MainWindow::updateInstrumentListColors()
{
	ui->instrumentListWidget->setStyleSheet(
				QString("QListWidget { color: %1; background: %2; }")
				.arg(palette_->ilistTextColor.name(QColor::HexArgb))
				.arg(palette_->ilistBackColor.name(QColor::HexArgb))
				+ QString("QListWidget::item:hover { color: %1; background: %2; }")
				.arg(palette_->ilistTextColor.name(QColor::HexArgb))
				.arg(palette_->ilistHovBackColor.name(QColor::HexArgb))
				+ QString("QListWidget::item:selected { color: %1; background: %2; }")
				.arg(palette_->ilistTextColor.name(QColor::HexArgb))
				.arg(palette_->ilistSelBackColor.name(QColor::HexArgb))
				+ QString("QListWidget::item:selected:hover { color: %1; background: %2; }")
				.arg(palette_->ilistTextColor.name(QColor::HexArgb))
				.arg(palette_->ilistHovSelBackColor.name(QColor::HexArgb)));
}

/********** MIDI **********/
void MainWindow::midiThreadReceivedEvent(double delay, const uint8_t *msg, size_t len, void *userData)
{
	MainWindow *self = reinterpret_cast<MainWindow *>(userData);

	Q_UNUSED(delay)

	// Note-On/Note-Off
	if (len == 3 && (msg[0] & 0xe0) == 0x80) {
		uint8_t status = msg[0];
		uint8_t key = msg[1];
		uint8_t velocity = msg[2];
		QMetaMethod method = self->metaObject()->method(self->midiKeyEventMethod_);
		method.invoke(self, Qt::QueuedConnection,
					  Q_ARG(uchar, status), Q_ARG(uchar, key), Q_ARG(uchar, velocity));
	}
	// Program change
	else if (len == 2 && (msg[0] & 0xf0) == 0xc0) {
		uint8_t status = msg[0];
		uint8_t program = msg[1];
		QMetaMethod method = self->metaObject()->method(self->midiProgramEventMethod_);
		method.invoke(self, Qt::QueuedConnection,
					  Q_ARG(uchar, status), Q_ARG(uchar, program));
	}
}

void MainWindow::midiKeyEvent(uchar status, uchar key, uchar velocity)
{
	bool release = ((status & 0xf0) == 0x80) || velocity == 0;
	int k = static_cast<int>(key) - 12;

	octave_->setValue(k / 12);
	bt_->jamKeyOff(k); // possibility to recover on stuck note
	if (!release) bt_->jamKeyOn(k);
}

void MainWindow::midiProgramEvent(uchar status, uchar program)
{
	Q_UNUSED(status)
	int row = findRowFromInstrumentList(program);
	ui->instrumentListWidget->setCurrentRow(row);
}

/********** Instrument list **********/
void MainWindow::addInstrument()
{
	switch (bt_->getCurrentTrackAttribute().source) {
	case SoundSource::FM:
	case SoundSource::SSG:
	{
		auto& list = ui->instrumentListWidget;

		int num = bt_->findFirstFreeInstrumentNumber();
		if (num == -1) return;	// Maximum count check

		QString name = tr("Instrument %1").arg(num);
		bt_->addInstrument(num, name.toUtf8().toStdString());

		TrackAttribute attrib = bt_->getCurrentTrackAttribute();
		comStack_->push(new AddInstrumentQtCommand(list, num, name, attrib.source, instForms_));
		ui->instrumentListWidget->setCurrentRow(num);
		break;
	}
	case SoundSource::DRUM:
		break;
	}
}

void MainWindow::removeInstrument(int row)
{
	if (row < 0) return;

	auto& list = ui->instrumentListWidget;
	int num = list->item(row)->data(Qt::UserRole).toInt();

	bt_->removeInstrument(num);
	comStack_->push(new RemoveInstrumentQtCommand(list, num, row, instForms_));
}

void MainWindow::editInstrument()
{
	auto item = ui->instrumentListWidget->currentItem();
	int num = item->data(Qt::UserRole).toInt();
	instForms_->showForm(num);
}

int MainWindow::findRowFromInstrumentList(int instNum)
{
	auto& list = ui->instrumentListWidget;
	int row = 0;
	for (; row < list->count(); ++row) {
		auto item = list->item(row);
		if (item->data(Qt::UserRole).toInt() == instNum) break;
	}
	return row;
}

void MainWindow::renameInstrument()
{
	auto list = ui->instrumentListWidget;
	auto item = list->currentItem();
	int num = item->data(Qt::UserRole).toInt();
	QString oldName = instForms_->getFormInstrumentName(num);
	auto line = new QLineEdit(oldName);

	QObject::connect(line, &QLineEdit::editingFinished,
					 this, [&, item, list, num, oldName] {
		QString newName = qobject_cast<QLineEdit*>(list->itemWidget(item))->text();
		list->removeItemWidget(item);
		bt_->setInstrumentName(num, newName.toUtf8().toStdString());
		int row = findRowFromInstrumentList(num);
		comStack_->push(new ChangeInstrumentNameQtCommand(list, num, row, instForms_, oldName, newName));
	});

	ui->instrumentListWidget->setItemWidget(item, line);

	line->selectAll();
	line->setFocus();
}

void MainWindow::cloneInstrument()
{
	int num = bt_->findFirstFreeInstrumentNumber();
	if (num == -1) return;

	int refNum = ui->instrumentListWidget->currentItem()->data(Qt::UserRole).toInt();
	// KEEP CODE ORDER //
	bt_->cloneInstrument(num, refNum);
	comStack_->push(new CloneInstrumentQtCommand(
						ui->instrumentListWidget, num, refNum, instForms_));
	//----------//
}

void MainWindow::deepCloneInstrument()
{
	int num = bt_->findFirstFreeInstrumentNumber();
	if (num == -1) return;

	int refNum = ui->instrumentListWidget->currentItem()->data(Qt::UserRole).toInt();
	// KEEP CODE ORDER //
	bt_->deepCloneInstrument(num, refNum);
	comStack_->push(new DeepCloneInstrumentQtCommand(
						ui->instrumentListWidget, num, refNum, instForms_));
	//----------//
}

void MainWindow::loadInstrument()
{
	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QStringList filters {
		tr("BambooTracker instrument (*.bti)"),
		tr("DefleMask preset (*.dmp)"),
		tr("TFM Music Maker instrument (*.tfi)"),
		tr("VGM Music Maker instrument (*.vgi)"),
		tr("WOPN instrument (*.opni)"),
		tr("Gens KMod dump (*.y12)"),
		tr("MVSTracker instrument (*.ins)")
	};
	QString defaultFilter = filters.at(config_.lock()->getInstrumentOpenFormat());

	QString file = QFileDialog::getOpenFileName(this, tr("Open instrument"), (dir.isEmpty() ? "./" : dir),
												filters.join(";;"), &defaultFilter);
	if (file.isNull()) return;

	int index = getSelectedFileFilter(file, filters);
	if (index != -1) config_.lock()->setInstrumentOpenFormat(index);


	funcLoadInstrument(file);
}

void MainWindow::funcLoadInstrument(QString file)
{
	int n = bt_->findFirstFreeInstrumentNumber();
	if (n == -1) QMessageBox::critical(this, tr("Error"), tr("Failed to load instrument."));

	try {
		bt_->loadInstrument(file.toStdString(), n);
		auto inst = bt_->getInstrument(n);
		auto name = inst->getName();
		comStack_->push(new AddInstrumentQtCommand(
							ui->instrumentListWidget, n,
							QString::fromUtf8(name.c_str(), static_cast<int>(name.length())),
							inst->getSoundSource(), instForms_));
		ui->instrumentListWidget->setCurrentRow(n);
		config_.lock()->setWorkingDirectory(QFileInfo(file).dir().path().toStdString());
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
	}
}

void MainWindow::saveInstrument()
{
	int n = ui->instrumentListWidget->currentItem()->data(Qt::UserRole).toInt();
	auto nameStd = bt_->getInstrument(n)->getName();
	QString name = QString::fromUtf8(nameStd.c_str(), static_cast<int>(nameStd.length()));

	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString file = QFileDialog::getSaveFileName(
					   this, tr("Save instrument"),
					   QString("%1/%2.bti").arg(dir.isEmpty() ? "." : dir, name),
					   tr("BambooTracker instrument file (*.bti)"));
	if (file.isNull()) return;
	if (!file.endsWith(".bti")) file += ".bti";	// For linux

	try {
		bt_->saveInstrument(file.toStdString(), n);
		config_.lock()->setWorkingDirectory(QFileInfo(file).dir().path().toStdString());
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
	}
}

void MainWindow::importInstrumentsFromBank()
{
	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QStringList filters {
		tr("BambooTracker bank (*.btb)"),
		tr("WOPN bank (*.wopn)")
	};
	QString defaultFilter = filters.at(config_.lock()->getBankOpenFormat());

	QString file = QFileDialog::getOpenFileName(this, tr("Open bank"), (dir.isEmpty() ? "./" : dir),
												filters.join(";;"), &defaultFilter);
	if (file.isNull()) {
		return;
	}
	else {
		int index = getSelectedFileFilter(file, filters);
		if (index != -1) config_.lock()->setBankOpenFormat(index);
	}

	funcImportInstrumentsFromBank(file);
}

void MainWindow::funcImportInstrumentsFromBank(QString file)
{
	std::unique_ptr<AbstractBank> bank;
	try {
		bank.reset(BankIO::loadBank(file.toStdString()));
		config_.lock()->setWorkingDirectory(QFileInfo(file).dir().path().toStdString());
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
		return;
	}

	InstrumentSelectionDialog dlg(*bank, tr("Select instruments to load:"), this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	QVector<size_t> selection = dlg.currentInstrumentSelection();
	if (selection.empty()) return;

	try {
		int lastNum = ui->instrumentListWidget->currentRow();
		for (size_t index : selection) {
			int n = bt_->findFirstFreeInstrumentNumber();
			if (n == -1){
				QMessageBox::critical(this, tr("Error"), tr("Failed to load instrument."));
				ui->instrumentListWidget->setCurrentRow(lastNum);
				return;
			}

			bt_->importInstrument(*bank, index, n);

			auto inst = bt_->getInstrument(n);
			auto name = inst->getName();
			comStack_->push(new AddInstrumentQtCommand(
								ui->instrumentListWidget, n,
								QString::fromUtf8(name.c_str(), static_cast<int>(name.length())),
								inst->getSoundSource(), instForms_));
			lastNum = n;
		}
		ui->instrumentListWidget->setCurrentRow(lastNum);
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
	}
}

void MainWindow::exportInstrumentsToBank()
{
	std::shared_ptr<BtBank> bank(std::make_shared<BtBank>(bt_->getInstrumentIndices(), bt_->getInstrumentNames()));

	InstrumentSelectionDialog dlg(*bank, tr("Select instruments to save:"), this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString file = QFileDialog::getSaveFileName(this, tr("Save bank"), (dir.isEmpty() ? "./" : dir),
												tr("BambooTracker bank file (*.btb)"));
	if (file.isNull()) return;

	std::vector<size_t> selection = dlg.currentInstrumentSelection().toStdVector();
	std::sort(selection.begin(), selection.end());
	if (selection.empty()) return;

	try {
		bt_->exportInstruments(file.toStdString(), selection);
		config_.lock()->setWorkingDirectory(QFileInfo(file).dir().path().toStdString());
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
	}
}

/********** Undo-Redo **********/
void MainWindow::undo()
{
	bt_->undo();
	comStack_->undo();
}

void MainWindow::redo()
{
	bt_->redo();
	comStack_->redo();
}

/********** Load data **********/
void MainWindow::loadModule()
{
	instForms_->clearAll();
	ui->instrumentListWidget->clear();
	on_instrumentListWidget_itemSelectionChanged();

	auto modTitle = bt_->getModuleTitle();
	ui->modTitleLineEdit->setText(
				QString::fromUtf8(modTitle.c_str(), static_cast<int>(modTitle.length())));
	ui->modTitleLineEdit->setCursorPosition(0);
	auto modAuthor = bt_->getModuleAuthor();
	ui->authorLineEdit->setText(
				QString::fromUtf8(modAuthor.c_str(), static_cast<int>(modAuthor.length())));
	ui->authorLineEdit->setCursorPosition(0);
	auto modCopyright = bt_->getModuleCopyright();
	ui->copyrightLineEdit->setText(
				QString::fromUtf8(modCopyright.c_str(), static_cast<int>(modCopyright.length())));
	ui->copyrightLineEdit->setCursorPosition(0);
	ui->songNumSpinBox->setMaximum(static_cast<int>(bt_->getSongCount()) - 1);
	highlight1_->setValue(static_cast<int>(bt_->getModuleStepHighlight1Distance()));
	highlight2_->setValue(static_cast<int>(bt_->getModuleStepHighlight2Distance()));

	for (auto& idx : bt_->getInstrumentIndices()) {
		auto inst = bt_->getInstrument(idx);
		auto name = inst->getName();
		comStack_->push(new AddInstrumentQtCommand(
							ui->instrumentListWidget, idx,
							QString::fromUtf8(name.c_str(), static_cast<int>(name.length())),
							inst->getSoundSource(), instForms_));
	}

	isSavedModBefore_ = false;

	loadSong();

	// Set tick frequency
	stream_->setInterruption(bt_->getModuleTickFrequency());
	if (timer_) timer_->setInterval(1000000 / bt_->getModuleTickFrequency());
	statusIntr_->setText(QString::number(bt_->getModuleTickFrequency()) + QString("Hz"));

	// Set mixer
	QString text;
	switch (bt_->getModuleMixerType()) {
	case MixerType::UNSPECIFIED:
		bt_->setMasterVolumeFM(config_.lock()->getMixerVolumeFM());
		bt_->setMasterVolumeSSG(config_.lock()->getMixerVolumeSSG());
		text = tr("-");
		break;
	case MixerType::CUSTOM:
		bt_->setMasterVolumeFM(bt_->getModuleCustomMixerFMLevel());
		bt_->setMasterVolumeSSG(bt_->getModuleCustomMixerSSGLevel());
		text = tr("Custom");
		break;
	case MixerType::PC_9821_PC_9801_86:
		bt_->setMasterVolumeFM(0);
		bt_->setMasterVolumeSSG(-5.5);
		text = tr("PC-9821 with PC-9801-86");
		break;
	case MixerType::PC_9821_SPEAK_BOARD:
		bt_->setMasterVolumeFM(0);
		bt_->setMasterVolumeSSG(-3.0);
		text = tr("PC-9821 with Speak Board");
		break;
	case MixerType::PC_8801_VA2:
		bt_->setMasterVolumeFM(0);
		bt_->setMasterVolumeSSG(1.5);
		text = tr("PC-88VA2");
		break;
	case MixerType::PC_8801_MKII_SR:
		bt_->setMasterVolumeFM(0);
		bt_->setMasterVolumeSSG(2.5);
		text = tr("NEC PC-8801mkIISR");
		break;
	}
	statusMixer_->setText(text);

	// Clear records
	QApplication::clipboard()->clear();
	comStack_->clear();
	bt_->clearCommandHistory();
}

void MainWindow::openModule(QString file)
{
	try {
		freezeViews();
		if (!timer_) stream_->stop();

		BinaryContainer container;
		QFile fp(file);
		if (!fp.open(QIODevice::ReadOnly)) throw FileInputError(FileIO::FileType::Mod);
		QByteArray array = fp.readAll();
		fp.close();

		container.appendVector(std::vector<char>(array.begin(), array.end()));
		bt_->loadModule(container);
		bt_->setModulePath(file.toStdString());

		loadModule();

		config_.lock()->setWorkingDirectory(QFileInfo(file).dir().path().toStdString());
		changeFileHistory(file);
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
		// Init module
		freezeViews();
		bt_->makeNewModule();
		loadModule();
	}

	isModifiedForNotCommand_ = false;
	setWindowModified(false);
	if (!timer_) stream_->start();
	setInitialSelectedInstrument();
}

void MainWindow::loadSong()
{
	// Init position
	int songCnt = static_cast<int>(bt_->getSongCount());
	if (ui->songNumSpinBox->value() >= songCnt)
		bt_->setCurrentSongNumber(songCnt - 1);
	else
		bt_->setCurrentSongNumber(bt_->getCurrentSongNumber());
	bt_->setCurrentOrderNumber(0);
	bt_->setCurrentTrack(0);
	bt_->setCurrentStepNumber(0);

	// Init ui
	ui->orderList->unfreeze();
	ui->patternEditor->unfreeze();
	ui->orderList->onSongLoaded();
	ui->orderListGroupBox->setMaximumWidth(
				ui->orderListGroupBox->contentsMargins().left()
				+ ui->orderListGroupBox->layout()->contentsMargins().left()
				+ ui->orderList->maximumWidth()
				+ ui->orderListGroupBox->layout()->contentsMargins().right()
				+ ui->orderListGroupBox->contentsMargins().right());
	ui->patternEditor->onSongLoaded();

	int curSong = bt_->getCurrentSongNumber();
	ui->songNumSpinBox->setValue(curSong);
	auto title = bt_->getSongTitle(curSong);
	ui->songTitleLineEdit->setText(QString::fromUtf8(title.c_str(), static_cast<int>(title.length())));
	ui->songTitleLineEdit->setCursorPosition(0);
	switch (bt_->getSongStyle(curSong).type) {
	case SongType::Standard:		ui->songStyleLineEdit->setText(tr("Standard"));			break;
	case SongType::FM3chExpanded:	ui->songStyleLineEdit->setText(tr("FM3ch expanded"));	break;
	}
	ui->songStyleLineEdit->setCursorPosition(0);
	ui->tempoSpinBox->setValue(bt_->getSongTempo(curSong));
	ui->speedSpinBox->setValue(bt_->getSongSpeed(curSong));
	ui->patternSizeSpinBox->setValue(static_cast<int>(bt_->getDefaultPatternSize(curSong)));
	ui->grooveSpinBox->setValue(bt_->getSongGroove(curSong));
	ui->grooveSpinBox->setMaximum(static_cast<int>(bt_->getGrooveCount()) - 1);
	if (bt_->isUsedTempoInSong(curSong)) {
		ui->tempoSpinBox->setEnabled(true);
		ui->speedSpinBox->setEnabled(true);
		ui->grooveCheckBox->setChecked(false);
		ui->grooveSpinBox->setEnabled(false);
	}
	else {
		ui->tempoSpinBox->setEnabled(false);
		ui->speedSpinBox->setEnabled(false);
		ui->grooveCheckBox->setChecked(true);
		ui->grooveSpinBox->setEnabled(true);
	}

	setWindowTitle();
	switch (bt_->getSongStyle(bt_->getCurrentSongNumber()).type) {
	case SongType::Standard:		statusStyle_->setText(tr("Standard"));			break;
	case SongType::FM3chExpanded:	statusStyle_->setText(tr("FM3ch expanded"));	break;
	}
	statusPlayPos_->setText("00/00");
}

/********** Play song **********/
void MainWindow::startPlaySong()
{
	bt_->startPlaySong();
	lockControls(true);
	firstViewUpdateRequest_ = true;
}

void MainWindow::startPlayFromStart()
{
	bt_->startPlayFromStart();
	lockControls(true);
	firstViewUpdateRequest_ = true;
}

void MainWindow::startPlayPattern()
{
	bt_->startPlayPattern();
	lockControls(true);
	firstViewUpdateRequest_ = true;
}

void MainWindow::startPlayFromCurrentStep()
{
	bt_->startPlayFromCurrentStep();
	lockControls(true);
	firstViewUpdateRequest_ = true;
}

void MainWindow::stopPlaySong()
{
	bt_->stopPlaySong();
	lockControls(false);
	ui->patternEditor->onStoppedPlaySong();
	ui->orderList->onStoppedPlaySong();
}

void MainWindow::lockControls(bool isLock)
{
	ui->songNumSpinBox->setEnabled(!isLock);
}

/********** Octave change **********/
void MainWindow::changeOctave(bool upFlag)
{
	if (upFlag) octave_->stepUp();
	else octave_->stepDown();

	statusOctave_->setText(tr("Octave: %1").arg(bt_->getCurrentOctave()));
}

/********** Configuration change **********/
void MainWindow::changeConfiguration()
{
	// SCCI settings
	if (config_.lock()->getUseSCCI()) {
		stream_->stop();
		if (!timer_) {
			timer_ = std::make_unique<Timer>();
			timer_->setInterval(1000000 / bt_->getModuleTickFrequency());
			tickEventMethod_ = metaObject()->indexOfSlot("onNewTickSignaledRealChip()");
			Q_ASSERT(tickEventMethod_ != -1);
			timer_->setFunction([&]{
				QMetaMethod method = this->metaObject()->method(this->tickEventMethod_);
				method.invoke(this, Qt::QueuedConnection);
			});

			scciDll_->load();
			if (scciDll_->isLoaded()) {
				SCCIFUNC getSoundInterfaceManager = reinterpret_cast<SCCIFUNC>(
														scciDll_->resolve("getSoundInterfaceManager"));
				bt_->useSCCI(getSoundInterfaceManager ? getSoundInterfaceManager() : nullptr);
			}
			else {
				bt_->useSCCI(nullptr);
			}

			timer_->start();
		}
	}
	else {
		timer_.reset();
		bt_->useSCCI(nullptr);
		bool streamState = stream_->initialize(
							   config_.lock()->getSampleRate(),
							   config_.lock()->getBufferLength(),
							   bt_->getModuleTickFrequency(),
							   QString::fromUtf8(config_.lock()->getSoundAPI().c_str(),
												 static_cast<int>(config_.lock()->getSoundAPI().length())),
							   QString::fromUtf8(config_.lock()->getSoundDevice().c_str(),
												 static_cast<int>(config_.lock()->getSoundDevice().length())));
		if (!streamState) showStreamFailedDialog();
		stream_->start();
	}

	setMidiConfiguration();
	updateFonts();
	instForms_->updateByConfiguration();

	bt_->changeConfiguration(config_);

	ui->waveVisual->setVisible(config_.lock()->getShowWaveVisual());

	updateInstrumentListColors();

	update();
}

void MainWindow::setMidiConfiguration()
{
	MidiInterface &midiIntf = MidiInterface::instance();
	std::string midiInPortName = config_.lock()->getMidiInputPort();

	if (!midiInPortName.empty())
		midiIntf.openInputPortByName(midiInPortName);
	else if (midiIntf.supportsVirtualPort())
		midiIntf.openInputPort(~0u);
}

void MainWindow::updateFonts()
{
	ui->patternEditor->setFonts(
				QString::fromUtf8(config_.lock()->getPatternEditorHeaderFont().c_str(),
								  static_cast<int>(config_.lock()->getPatternEditorHeaderFont().length())),
				config_.lock()->getPatternEditorHeaderFontSize(),
				QString::fromUtf8(config_.lock()->getPatternEditorRowsFont().c_str(),
								  static_cast<int>(config_.lock()->getPatternEditorRowsFont().length())),
				config_.lock()->getPatternEditorRowsFontSize());
	ui->orderList->setFonts(
				QString::fromUtf8(config_.lock()->getOrderListHeaderFont().c_str(),
								  static_cast<int>(config_.lock()->getOrderListHeaderFont().length())),
				config_.lock()->getOrderListHeaderFontSize(),
				QString::fromUtf8(config_.lock()->getOrderListRowsFont().c_str(),
								  static_cast<int>(config_.lock()->getOrderListRowsFont().length())),
				config_.lock()->getOrderListRowsFontSize());
}

/********** History change **********/
void MainWindow::changeFileHistory(QString file)
{
	fileHistory_->addFile(file);
	for (int i = ui->menu_Recent_Files->actions().count() - 1; 1 < i; --i)
		ui->menu_Recent_Files->removeAction(ui->menu_Recent_Files->actions().at(i));
	for (size_t i = 0; i < fileHistory_->size(); ++i) {
		// Leave Before Qt5.7.0 style due to windows xp
		QAction* action = ui->menu_Recent_Files->addAction(QString("&%1 %2").arg(i + 1).arg(fileHistory_->at(i)));
		action->setData(fileHistory_->at(i));
	}
}

/********** Layout decypherer **********/
JamKey MainWindow::getJamKeyFromLayoutMapping(Qt::Key key) {
	std::shared_ptr<Configuration> configLocked = config_.lock();
	Configuration::KeyboardLayout selectedLayout = configLocked->getNoteEntryLayout();
	if (configLocked->mappingLayouts.find (selectedLayout) != configLocked->mappingLayouts.end()) {
		std::unordered_map<std::string, JamKey> selectedLayoutMapping = configLocked->mappingLayouts.at (selectedLayout);
		auto it = std::find_if(selectedLayoutMapping.begin(), selectedLayoutMapping.end(),
							   [key](const std::pair<std::string, JamKey>& t) -> bool {
			return (QKeySequence(key).matches(QKeySequence(QString::fromStdString(t.first))) == QKeySequence::ExactMatch);
		});
		if (it != selectedLayoutMapping.end()) {
			return (*it).second;
		}
		else {
			throw std::invalid_argument("Unmapped key");
		}
		//something has gone wrong, current layout has no layout map
		//TODO: handle cleanly?
	} else throw std::out_of_range("Unmapped Layout");
}

/******************************/
void MainWindow::setWindowTitle()
{
	int n = bt_->getCurrentSongNumber();
	auto filePathStd = bt_->getModulePath();
	auto songTitleStd = bt_->getSongTitle(n);
	QString filePath = QString::fromStdString(filePathStd);
	QString fileName = filePath.isEmpty() ? tr("Untitled") : QFileInfo(filePath).fileName();
	QString songTitle = QString::fromUtf8(songTitleStd.c_str(), static_cast<int>(songTitleStd.length()));
	if (songTitle.isEmpty()) songTitle = tr("Untitled");
	QMainWindow::setWindowTitle(QString("%1[*] [#%2 %3] - BambooTracker")
								.arg(fileName).arg(QString::number(n)).arg(songTitle));
}

void MainWindow::setModifiedTrue()
{
	isModifiedForNotCommand_ = true;
	setWindowModified(true);
}

void MainWindow::setInitialSelectedInstrument()
{
	if (bt_->getInstrumentIndices().empty()) {
		bt_->setCurrentInstrument(-1);
		statusInst_->setText(tr("No instrument"));
	}
	else {
		ui->instrumentListWidget->setCurrentRow(0);
	}
}

QString MainWindow::getModuleFileBaseName() const
{
	auto filePathStd = bt_->getModulePath();
	QString filePath = QString::fromStdString(filePathStd);
	return (filePath.isEmpty() ? tr("Untitled") : QFileInfo(filePath).completeBaseName());
}

int MainWindow::getSelectedFileFilter(QString& file, QStringList& filters) const
{
	QRegularExpression re(R"(\(\*\.(.+)\))");
	QString ex = QFileInfo(file).suffix();
	for (int i = 0; i < filters.size(); ++i)
		if (ex == re.match(filters[i]).captured(1)) return i;
	return -1;
}

/******************************/
/********** Instrument list events **********/
void MainWindow::on_instrumentListWidget_customContextMenuRequested(const QPoint &pos)
{
	auto& list = ui->instrumentListWidget;
	QPoint globalPos = list->mapToGlobal(pos);
	QMenu menu;

	// Leave Before Qt5.7.0 style due to windows xp
	menu.addAction(ui->actionNew_Instrument);
	menu.addAction(ui->actionRemove_Instrument);
	menu.addSeparator();
	menu.addAction(ui->actionRename_Instrument);
	menu.addSeparator();
	menu.addAction(ui->actionClone_Instrument);
	menu.addAction(ui->actionDeep_Clone_Instrument);
	menu.addSeparator();
	menu.addAction(ui->actionLoad_From_File);
	menu.addAction(ui->actionSave_To_File);
	menu.addSeparator();
	menu.addAction(ui->actionImport_From_Bank_File);
	menu.addAction(ui->actionExport_To_Bank_File);
	menu.addSeparator();
	menu.addAction(ui->actionEdit);

	menu.exec(globalPos);
}

void MainWindow::on_instrumentListWidget_itemDoubleClicked(QListWidgetItem *item)
{
	Q_UNUSED(item)
	editInstrument();
}

void MainWindow::onInstrumentListWidgetItemAdded(const QModelIndex &parent, int start, int end)
{
	Q_UNUSED(parent)
	Q_UNUSED(end)

	// Set core data to editor when add insrument
	int n = ui->instrumentListWidget->item(start)->data(Qt::UserRole).toInt();
	auto& form = instForms_->getForm(n);
	auto playFunc = [&](int stat) {
		switch (stat) {
		case -1:	stopPlaySong();				break;
		case 0:		startPlaySong();			break;
		case 1:		startPlayFromStart();		break;
		case 2:		startPlayPattern();			break;
		case 3:		startPlayFromCurrentStep();	break;
		default:	break;
		}
	};
	switch (instForms_->getFormInstrumentSoundSource(n)) {
	case SoundSource::FM:
	{
		auto fmForm = qobject_cast<InstrumentEditorFMForm*>(form.get());
		fmForm->setCore(bt_);
		fmForm->setConfiguration(config_.lock());
		fmForm->setColorPalette(palette_);
		fmForm->resize(config_.lock()->getInstrumentFMWindowWidth(),
					   config_.lock()->getInstrumentFMWindowHeight());

		QObject::connect(fmForm, &InstrumentEditorFMForm::envelopeNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMEnvelopeNumberChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::envelopeParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMEnvelopeParameterChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::lfoNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMLFONumberChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::lfoParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMLFOParameterChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::operatorSequenceNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMOperatorSequenceNumberChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::operatorSequenceParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMOperatorSequenceParameterChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::arpeggioNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMArpeggioNumberChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::arpeggioParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMArpeggioParameterChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::pitchNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMPitchNumberChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::pitchParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentFMPitchParameterChanged);
		QObject::connect(fmForm, &InstrumentEditorFMForm::jamKeyOnEvent,
						 this, &MainWindow::keyPressEvent, Qt::DirectConnection);
		QObject::connect(fmForm, &InstrumentEditorFMForm::jamKeyOffEvent,
						 this, &MainWindow::keyReleaseEvent, Qt::DirectConnection);
		QObject::connect(fmForm, &InstrumentEditorFMForm::octaveChanged,
						 this, &MainWindow::changeOctave, Qt::DirectConnection);
		QObject::connect(fmForm, &InstrumentEditorFMForm::modified,
						 this, &MainWindow::setModifiedTrue);
		QObject::connect(fmForm, &InstrumentEditorFMForm::playStatusChanged, this, playFunc);

		fmForm->installEventFilter(this);

		instForms_->onInstrumentFMEnvelopeNumberChanged();
		instForms_->onInstrumentFMLFONumberChanged();
		instForms_->onInstrumentFMOperatorSequenceNumberChanged();
		instForms_->onInstrumentFMArpeggioNumberChanged();
		instForms_->onInstrumentFMPitchNumberChanged();
		break;
	}
	case SoundSource::SSG:
	{
		auto ssgForm = qobject_cast<InstrumentEditorSSGForm*>(form.get());
		ssgForm->setCore(bt_);
		ssgForm->setConfiguration(config_.lock());
		ssgForm->setColorPalette(palette_);
		ssgForm->resize(config_.lock()->getInstrumentSSGWindowWidth(),
						config_.lock()->getInstrumentSSGWindowHeight());

		QObject::connect(ssgForm, &InstrumentEditorSSGForm::waveFormNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGWaveFormNumberChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::waveFormParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGWaveFormParameterChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::toneNoiseNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGToneNoiseNumberChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::toneNoiseParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGToneNoiseParameterChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::envelopeNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGEnvelopeNumberChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::envelopeParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGEnvelopeParameterChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::arpeggioNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGArpeggioNumberChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::arpeggioParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGArpeggioParameterChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::pitchNumberChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGPitchNumberChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::pitchParameterChanged,
						 instForms_.get(), &InstrumentFormManager::onInstrumentSSGPitchParameterChanged);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::jamKeyOnEvent,
						 this, &MainWindow::keyPressEvent, Qt::DirectConnection);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::jamKeyOffEvent,
						 this, &MainWindow::keyReleaseEvent, Qt::DirectConnection);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::octaveChanged,
						 this, &MainWindow::changeOctave, Qt::DirectConnection);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::modified,
						 this, &MainWindow::setModifiedTrue);
		QObject::connect(ssgForm, &InstrumentEditorSSGForm::playStatusChanged, this, playFunc);

		ssgForm->installEventFilter(this);

		instForms_->onInstrumentSSGWaveFormNumberChanged();
		instForms_->onInstrumentSSGToneNoiseNumberChanged();
		instForms_->onInstrumentSSGEnvelopeNumberChanged();
		instForms_->onInstrumentSSGArpeggioNumberChanged();
		instForms_->onInstrumentSSGPitchNumberChanged();
		break;
	}
	default:
		break;
	}
}

void MainWindow::on_instrumentListWidget_itemSelectionChanged()
{
	int num = (ui->instrumentListWidget->currentRow() == -1)
			  ? -1
			  : ui->instrumentListWidget->currentItem()->data(Qt::UserRole).toInt();
	bt_->setCurrentInstrument(num);

	if (num == -1) statusInst_->setText(tr("No instrument"));
	else statusInst_->setText(
				tr("Instrument: ") + QString("%1").arg(num, 2, 16, QChar('0')).toUpper());

	if (bt_->findFirstFreeInstrumentNumber() == -1) {    // Max size
		ui->actionNew_Instrument->setEnabled(false);
		ui->actionLoad_From_File->setEnabled(false);
		ui->actionImport_From_Bank_File->setEnabled(false);
	}
	else {
		switch (bt_->getCurrentTrackAttribute().source) {
		case SoundSource::DRUM:	ui->actionNew_Instrument->setEnabled(false);	break;
		default:	break;
		}
	}
	bool isEnabled = (num != -1);
	ui->actionRemove_Instrument->setEnabled(isEnabled);
	ui->actionClone_Instrument->setEnabled(isEnabled);
	ui->actionDeep_Clone_Instrument->setEnabled(isEnabled);
	ui->actionSave_To_File->setEnabled(isEnabled);
	ui->actionExport_To_Bank_File->setEnabled(isEnabled);
	ui->actionRename_Instrument->setEnabled(isEnabled);
	ui->actionEdit->setEnabled(isEnabled);
}

void MainWindow::on_grooveCheckBox_stateChanged(int arg1)
{
	if (arg1 == Qt::Checked) {
		ui->tempoSpinBox->setEnabled(false);
		ui->speedSpinBox->setEnabled(false);
		ui->grooveSpinBox->setEnabled(true);
		bt_->toggleTempoOrGrooveInSong(bt_->getCurrentSongNumber(), false);
	}
	else {
		ui->tempoSpinBox->setEnabled(true);
		ui->speedSpinBox->setEnabled(true);
		ui->grooveSpinBox->setEnabled(false);
		bt_->toggleTempoOrGrooveInSong(bt_->getCurrentSongNumber(), true);
	}

	setModifiedTrue();
}

void MainWindow::on_actionExit_triggered()
{
	close();
}

void MainWindow::on_actionUndo_triggered()
{
	undo();
}

void MainWindow::on_actionRedo_triggered()
{
	redo();
}

void MainWindow::on_actionCut_triggered()
{
	if (isEditedPattern_) ui->patternEditor->cutSelectedCells();
}

void MainWindow::on_actionCopy_triggered()
{
	if (isEditedPattern_) ui->patternEditor->copySelectedCells();
	else if (isEditedOrder_) ui->orderList->copySelectedCells();
}

void MainWindow::on_actionPaste_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onPastePressed();
	else if (isEditedOrder_) ui->orderList->onPastePressed();
}

void MainWindow::on_actionDelete_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onDeletePressed();
	else if (isEditedOrder_) ui->orderList->deleteOrder();
	else if (isEditedInstList_) on_actionRemove_Instrument_triggered();
}

void MainWindow::updateMenuByPattern()
{
	isEditedPattern_ = true;
	isEditedOrder_ = false;
	isEditedInstList_ = false;

	if (bt_->isJamMode()) {
		// Edit
		ui->actionPaste->setEnabled(false);
		ui->actionMix->setEnabled(false);
		ui->actionOverwrite->setEnabled(false);
		ui->actionDelete->setEnabled(false);
		// Pattern
		ui->actionInterpolate->setEnabled(false);
		ui->actionReverse->setEnabled(false);
		ui->actionReplace_Instrument->setEnabled(false);
		ui->actionExpand->setEnabled(false);
		ui->actionShrink->setEnabled(false);
		ui->actionDecrease_Note->setEnabled(false);
		ui->actionIncrease_Note->setEnabled(false);
		ui->actionDecrease_Octave->setEnabled(false);
		ui->actionIncrease_Octave->setEnabled(false);
	}
	else {
		// Edit
		bool enabled = QApplication::clipboard()->text().startsWith("PATTERN_");
		ui->actionPaste->setEnabled(enabled);
		ui->actionMix->setEnabled(enabled);
		ui->actionOverwrite->setEnabled(enabled);
		ui->actionDelete->setEnabled(true);
		// Pattern
		ui->actionInterpolate->setEnabled(isSelectedPO_);
		ui->actionReverse->setEnabled(isSelectedPO_);
		ui->actionReplace_Instrument->setEnabled(
					isSelectedPO_ && ui->instrumentListWidget->currentRow() != -1);
		ui->actionExpand->setEnabled(isSelectedPO_);
		ui->actionShrink->setEnabled(isSelectedPO_);
		ui->actionDecrease_Note->setEnabled(true);
		ui->actionIncrease_Note->setEnabled(true);
		ui->actionDecrease_Octave->setEnabled(true);
		ui->actionIncrease_Octave->setEnabled(true);
	}
}

void MainWindow::updateMenuByOrder()
{
	isEditedPattern_ = false;
	isEditedOrder_ = true;
	isEditedInstList_ = false;

	// Edit
	bool enabled = QApplication::clipboard()->text().startsWith("ORDER_");
	ui->actionPaste->setEnabled(enabled);
	ui->actionMix->setEnabled(false);
	ui->actionOverwrite->setEnabled(false);
	ui->actionDelete->setEnabled(true);
	// Song
	bool canAdd = bt_->canAddNewOrder(bt_->getCurrentSongNumber());
	ui->actionInsert_Order->setEnabled(canAdd);
	//ui->actionRemove_Order->setEnabled(true);
	ui->actionDuplicate_Order->setEnabled(canAdd);
	//ui->actionMove_Order_Up->setEnabled(true);
	//ui->actionMove_Order_Down->setEnabled(true);
	ui->actionClone_Patterns->setEnabled(canAdd);
	ui->actionClone_Order->setEnabled(canAdd);
	// Pattern
	ui->actionInterpolate->setEnabled(false);
	ui->actionReverse->setEnabled(false);
	ui->actionReplace_Instrument->setEnabled(false);
	ui->actionExpand->setEnabled(false);
	ui->actionShrink->setEnabled(false);
	ui->actionDecrease_Note->setEnabled(false);
	ui->actionIncrease_Note->setEnabled(false);
	ui->actionDecrease_Octave->setEnabled(false);
	ui->actionIncrease_Octave->setEnabled(false);
}

void MainWindow::updateMenuByInstrumentList()
{
	isEditedPattern_ = false;
	isEditedOrder_ = false;
	isEditedInstList_ = true;

	// Edit
	ui->actionPaste->setEnabled(false);
	ui->actionMix->setEnabled(false);
	ui->actionOverwrite->setEnabled(false);
	ui->actionDelete->setEnabled(true);

	// Pattern
	ui->actionInterpolate->setEnabled(false);
	ui->actionReverse->setEnabled(false);
	ui->actionReplace_Instrument->setEnabled(false);
	ui->actionExpand->setEnabled(false);
	ui->actionShrink->setEnabled(false);
	ui->actionDecrease_Note->setEnabled(false);
	ui->actionIncrease_Note->setEnabled(false);
	ui->actionDecrease_Octave->setEnabled(false);
	ui->actionIncrease_Octave->setEnabled(false);
}

void MainWindow::updateMenuByPatternAndOrderSelection(bool isSelected)
{
	isSelectedPO_ = isSelected;

	if (bt_->isJamMode()) {
		// Edit
		ui->actionCopy->setEnabled(false);
		ui->actionCut->setEnabled(false);
		// Pattern
		ui->actionInterpolate->setEnabled(false);
		ui->actionReverse->setEnabled(false);
		ui->actionReplace_Instrument->setEnabled(false);
		ui->actionExpand->setEnabled(false);
		ui->actionShrink->setEnabled(false);
	}
	else {
		// Edit
		ui->actionCopy->setEnabled(isSelected);
		ui->actionCut->setEnabled(isEditedPattern_ ? isSelected : false);
		// Pattern
		bool enabled = (isEditedPattern_ && isEditedPattern_) ? isSelected : false;
		ui->actionInterpolate->setEnabled(enabled);
		ui->actionReverse->setEnabled(enabled);
		ui->actionReplace_Instrument->setEnabled(
					enabled && ui->instrumentListWidget->currentRow() != -1);
		ui->actionExpand->setEnabled(enabled);
		ui->actionShrink->setEnabled(enabled);
	}
}

void MainWindow::on_actionAll_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onSelectPressed(1);
	else if (isEditedOrder_) ui->orderList->onSelectPressed(1);
}

void MainWindow::on_actionNone_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onSelectPressed(0);
	else if (isEditedOrder_) ui->orderList->onSelectPressed(0);
}

void MainWindow::on_actionDecrease_Note_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onTransposePressed(false, false);
}

void MainWindow::on_actionIncrease_Note_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onTransposePressed(false, true);
}

void MainWindow::on_actionDecrease_Octave_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onTransposePressed(true, false);
}

void MainWindow::on_actionIncrease_Octave_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onTransposePressed(true, true);
}

void MainWindow::on_actionInsert_Order_triggered()
{
	ui->orderList->insertOrderBelow();
}

void MainWindow::on_actionRemove_Order_triggered()
{
	ui->orderList->deleteOrder();
}

void MainWindow::on_actionModule_Properties_triggered()
{
	ModulePropertiesDialog dialog(bt_, config_.lock()->getMixerVolumeFM(), config_.lock()->getMixerVolumeSSG());
	if (dialog.exec() == QDialog::Accepted
			&& showUndoResetWarningDialog(tr("Do you want to change song properties?"))) {
		int instRow = ui->instrumentListWidget->currentRow();
		bt_->stopPlaySong();
		lockControls(false);
		dialog.onAccepted();
		freezeViews();
		if (!timer_) stream_->stop();
		loadModule();
		setModifiedTrue();
		setWindowTitle();
		ui->instrumentListWidget->setCurrentRow(instRow);
		if (!timer_) stream_->start();
	}
}

void MainWindow::on_actionNew_Instrument_triggered()
{
	addInstrument();
}

void MainWindow::on_actionRemove_Instrument_triggered()
{
	removeInstrument(ui->instrumentListWidget->currentRow());
}

void MainWindow::on_actionClone_Instrument_triggered()
{
	cloneInstrument();
}

void MainWindow::on_actionDeep_Clone_Instrument_triggered()
{
	deepCloneInstrument();
}

void MainWindow::on_actionEdit_triggered()
{
	editInstrument();
}

void MainWindow::on_actionPlay_triggered()
{
	startPlaySong();
}

void MainWindow::on_actionPlay_Pattern_triggered()
{
	startPlayPattern();
}

void MainWindow::on_actionPlay_From_Start_triggered()
{
	startPlayFromStart();
}

void MainWindow::on_actionPlay_From_Cursor_triggered()
{
	startPlayFromCurrentStep();
}

void MainWindow::on_actionStop_triggered()
{
	stopPlaySong();
}

void MainWindow::on_actionEdit_Mode_triggered()
{
	bt_->toggleJamMode();
	ui->orderList->changeEditable();
	ui->patternEditor->changeEditable();

	if (isEditedOrder_) updateMenuByOrder();
	else if (isEditedPattern_) updateMenuByPattern();
	updateMenuByPatternAndOrderSelection(isSelectedPO_);

	if (bt_->isJamMode()) statusDetail_->setText(tr("Change to jam mode"));
	else statusDetail_->setText(tr("Change to edit mode"));
}

void MainWindow::on_actionToggle_Track_triggered()
{
	ui->patternEditor->onToggleTrackPressed();
}

void MainWindow::on_actionSolo_Track_triggered()
{
	ui->patternEditor->onSoloTrackPressed();
}

void MainWindow::on_actionKill_Sound_triggered()
{
	bt_->killSound();
}

void MainWindow::on_actionAbout_triggered()
{
	QMessageBox box(QMessageBox::NoIcon,
					tr("About"),
					QString("<h2>BambooTracker v")
					+ QString::fromStdString(Version::ofApplicationInString())
					+ QString("</h2>")
					+ tr("<b>YM2608 (OPNA) Music Tracker<br>"
						 "Copyright (C) 2018, 2019 Rerrah</b><br>"
						 "<hr>"
						 "Libraries:<br>"
						 "- libOPNMIDI by (C) Vitaly Novichkov (MIT License part)<br>"
						 "- MAME (MAME License)<br>"
						 "- nowide by (C) Artyom Beilis (BSL v1.0)<br>"
						 "- Nuked OPN-MOD by (C) Alexey Khokholov (Nuke.YKT)<br>"
						 "and (C) Jean Pierre Cimalando (LGPL v2.1)<br>"
						 "- RtAudio by (C) Gary P. Scavone (RtAudio License)<br>"
						 "- RtMidi by (C) Gary P. Scavone (RtMidi License)<br>"
						 "- SCCI (SCCI License)<br>"
						 "- Silk icon set 1.3 by (C) Mark James (CC BY 2.5)<br>"
						 "- Qt (GPL v2+ or LGPL v3)<br>"
						 "- VGMPlay by (C) Valley Bell (GPL v2)<br>"
						 "<br>"
						 "Also see changelog which lists contributors."),
					QMessageBox::Ok,
					this);
	box.setIconPixmap(QIcon(":/icon/app_icon").pixmap(QSize(44, 44)));
	box.exec();
}

void MainWindow::on_actionFollow_Mode_triggered()
{
	bt_->setFollowPlay(ui->actionFollow_Mode->isChecked());
	config_.lock()->setFollowMode(ui->actionFollow_Mode->isChecked());

	ui->orderList->onFollowModeChanged();
	ui->patternEditor->onFollowModeChanged();
}

void MainWindow::on_actionGroove_Settings_triggered()
{
	std::vector<std::vector<int>> seqs;
	for (size_t i = 0; i < bt_->getGrooveCount(); ++i) {
		seqs.push_back(bt_->getGroove(static_cast<int>(i)));
	}

	GrooveSettingsDialog diag;
	diag.setGrooveSquences(seqs);
	if (diag.exec() == QDialog::Accepted) {
		bt_->stopPlaySong();
		lockControls(false);
		bt_->setGrooves(diag.getGrooveSequences());
		ui->grooveSpinBox->setMaximum(static_cast<int>(bt_->getGrooveCount()) - 1);
		setModifiedTrue();
	}
}

void MainWindow::on_actionConfiguration_triggered()
{
	ConfigurationDialog diag(config_.lock(), palette_, stream_->getCurrentBackend(), stream_->getAvailableBackends());
	QObject::connect(&diag, &ConfigurationDialog::applyPressed, this, &MainWindow::changeConfiguration);

	if (diag.exec() == QDialog::Accepted) {
		bt_->stopPlaySong();
		changeConfiguration();
		ConfigurationHandler::saveConfiguration(config_.lock());
		ColorPaletteHandler::savePalette(palette_);
		lockControls(false);
	}
}

void MainWindow::on_actionExpand_triggered()
{
	ui->patternEditor->onExpandPressed();
}

void MainWindow::on_actionShrink_triggered()
{
	ui->patternEditor->onShrinkPressed();
}

void MainWindow::on_actionDuplicate_Order_triggered()
{
	ui->orderList->onDuplicatePressed();
}

void MainWindow::on_actionMove_Order_Up_triggered()
{
	ui->orderList->onMoveOrderPressed(true);
}

void MainWindow::on_actionMove_Order_Down_triggered()
{
	ui->orderList->onMoveOrderPressed(false);
}

void MainWindow::on_actionClone_Patterns_triggered()
{
	ui->orderList->onClonePatternsPressed();
}

void MainWindow::on_actionClone_Order_triggered()
{
	ui->orderList->onCloneOrderPressed();
}

void MainWindow::on_actionNew_triggered()
{
	if (isWindowModified()) {
		auto modTitleStd = bt_->getModuleTitle();
		QString modTitle = QString::fromUtf8(modTitleStd.c_str(), static_cast<int>(modTitleStd.length()));
		if (modTitle.isEmpty()) modTitle = tr("Untitled");
		QMessageBox dialog(QMessageBox::Warning,
						   "BambooTracker",
						   tr("Save changes to %1?").arg(modTitle),
						   QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
		switch (dialog.exec()) {
		case QMessageBox::Yes:
			if (!on_actionSave_triggered()) return;
			break;
		case QMessageBox::No:
			break;
		case QMessageBox::Cancel:
			return;
		default:
			break;
		}
	}

	bt_->stopPlaySong();
	lockControls(false);
	freezeViews();
	if (!timer_) stream_->stop();
	bt_->makeNewModule();
	loadModule();
	setInitialSelectedInstrument();
	isModifiedForNotCommand_ = false;
	setWindowModified(false);
	if (!timer_) stream_->start();
}

void MainWindow::on_actionComments_triggered()
{
	auto comment = bt_->getModuleComment();
	CommentEditDialog diag(QString::fromUtf8(comment.c_str(), static_cast<int>(comment.length())));
	if (diag.exec() == QDialog::Accepted) {
		bt_->setModuleComment(diag.getComment().toUtf8().toStdString());
		setModifiedTrue();
	}
}

bool MainWindow::on_actionSave_triggered()
{
	auto path = QString::fromStdString(bt_->getModulePath());
	if (!path.isEmpty() && QFileInfo::exists(path) && QFileInfo(path).isFile()) {
		if (!isSavedModBefore_ && config_.lock()->getBackupModules()) {
			if (!QFile::copy(path, path + ".bak")) {
				QMessageBox::critical(this, tr("Error"), tr("Failed to backup module."));
				return false;
			}
		}

		try {
			BinaryContainer container;
			bt_->saveModule(container);

			QFile fp(path);
			if (!fp.open(QIODevice::WriteOnly)) throw FileOutputError(FileIO::FileType::Mod);
			fp.write(container.getPointer(), container.size());
			fp.close();

			isModifiedForNotCommand_ = false;
			isSavedModBefore_ = true;
			setWindowModified(false);
			setWindowTitle();
			return true;
		}
		catch (std::exception& e) {
			QMessageBox::critical(this, tr("Error"), e.what());
			return false;
		}
	}
	else {
		return on_actionSave_As_triggered();
	}
}

bool MainWindow::on_actionSave_As_triggered()
{
	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString file = QFileDialog::getSaveFileName(
					   this, tr("Save module"),
					   QString("%1/%2.btm").arg(dir.isEmpty() ? "." : dir, getModuleFileBaseName()),
					   tr("BambooTracker module file (*.btm)"));
	if (file.isNull()) return false;
	if (!file.endsWith(".btm")) file += ".btm";	// For linux

	if (QFile::exists(file)) {	// Already exists
		if (!isSavedModBefore_ && config_.lock()->getBackupModules()) {
			if (!QFile::copy(file, file + ".bak")) {
				QMessageBox::critical(this, tr("Error"), tr("Failed to backup module."));
				return false;
			}
		}
	}

	bt_->setModulePath(file.toStdString());
	try {
		BinaryContainer container;
		bt_->saveModule(container);

		QFile fp(file);
		if (!fp.open(QIODevice::WriteOnly)) throw FileOutputError(FileIO::FileType::Mod);
		fp.write(container.getPointer(), container.size());
		fp.close();

		isModifiedForNotCommand_ = false;
		isSavedModBefore_ = true;
		setWindowModified(false);
		setWindowTitle();
		config_.lock()->setWorkingDirectory(QFileInfo(file).dir().path().toStdString());
		changeFileHistory(file);
		return true;
	}
	catch (std::exception& e) {
		QMessageBox::critical(this, tr("Error"), e.what());
		return false;
	}
}

void MainWindow::on_actionOpen_triggered()
{
	if (isWindowModified()) {
		auto modTitleStd = bt_->getModuleTitle();
		QString modTitle = QString::fromUtf8(modTitleStd.c_str(), static_cast<int>(modTitleStd.length()));
		if (modTitle.isEmpty()) modTitle = tr("Untitled");
		QMessageBox dialog(QMessageBox::Warning,
						   "BambooTracker",
						   tr("Save changes to %1?").arg(modTitle),
						   QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
		switch (dialog.exec()) {
		case QMessageBox::Yes:
			if (!on_actionSave_triggered()) return;
			break;
		case QMessageBox::No:
			break;
		case QMessageBox::Cancel:
			return;
		default:
			break;
		}
	}

	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString file = QFileDialog::getOpenFileName(this, tr("Open module"), (dir.isEmpty() ? "./" : dir),
												tr("BambooTracker module file (*.btm)"));
	if (file.isNull()) return;

	bt_->stopPlaySong();
	lockControls(false);

	openModule(file);
}

void MainWindow::on_actionLoad_From_File_triggered()
{
	loadInstrument();
}

void MainWindow::on_actionSave_To_File_triggered()
{
	saveInstrument();
}

void MainWindow::on_actionImport_From_Bank_File_triggered()
{
	importInstrumentsFromBank();
}

void MainWindow::on_actionInterpolate_triggered()
{
	ui->patternEditor->onInterpolatePressed();
}

void MainWindow::on_actionReverse_triggered()
{
	ui->patternEditor->onReversePressed();
}

void MainWindow::on_actionReplace_Instrument_triggered()
{
	ui->patternEditor->onReplaceInstrumentPressed();
}

void MainWindow::on_actionRow_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onSelectPressed(2);
	else if (isEditedOrder_) ui->orderList->onSelectPressed(2);
}

void MainWindow::on_actionColumn_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onSelectPressed(3);
	else if (isEditedOrder_) ui->orderList->onSelectPressed(3);
}

void MainWindow::on_actionPattern_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onSelectPressed(4);
	else if (isEditedOrder_) ui->orderList->onSelectPressed(4);
}

void MainWindow::on_actionOrder_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onSelectPressed(5);
	else if (isEditedOrder_) ui->orderList->onSelectPressed(5);
}

void MainWindow::on_actionRemove_Unused_Instruments_triggered()
{
	if (showUndoResetWarningDialog(tr("Do you want to remove all unused instruments?"))) {
		bt_->stopPlaySong();
		lockControls(false);

		auto list = ui->instrumentListWidget;
		for (auto& n : bt_->getUnusedInstrumentIndices()) {
			for (int i = 0; i < list->count(); ++i) {
				if (list->item(i)->data(Qt::UserRole).toInt() == n) {
					removeInstrument(i);
				}
			}
		}
		bt_->clearUnusedInstrumentProperties();
		bt_->clearCommandHistory();
		comStack_->clear();
		setModifiedTrue();
	}
}

void MainWindow::on_actionRemove_Unused_Patterns_triggered()
{
	if (showUndoResetWarningDialog(tr("Do you want to remove all unused patterns?"))) {
		bt_->stopPlaySong();
		lockControls(false);

		bt_->clearUnusedPatterns();
		bt_->clearCommandHistory();
		comStack_->clear();
		setModifiedTrue();
	}
}

void MainWindow::on_actionWAV_triggered()
{
	WaveExportSettingsDialog diag;
	if (diag.exec() != QDialog::Accepted) return;

	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString path = QFileDialog::getSaveFileName(
					   this, tr("Export to wav"),
					   QString("%1/%2.wav").arg(dir.isEmpty() ? "." : dir, getModuleFileBaseName()),
					   "WAV signed 16-bit PCM (*.wav)");
	if (path.isNull()) return;
	if (!path.endsWith(".wav")) path += ".wav";	// For linux

	QProgressDialog progress(
				tr("Export to WAV"),
				tr("Cancel"),
				0,
				static_cast<int>(bt_->getAllStepCount(bt_->getCurrentSongNumber(), diag.getLoopCount())) + 3
				);
	progress.setValue(0);
	progress.setWindowFlags(progress.windowFlags()
							& ~Qt::WindowContextHelpButtonHint
							& ~Qt::WindowCloseButtonHint);
	progress.show();

	bt_->stopPlaySong();
	lockControls(false);
	stream_->stop();

	try {
		BinaryContainer container;
		auto bar = [&progress]() -> bool {
				   QApplication::processEvents();
				   progress.setValue(progress.value() + 1);
				   return progress.wasCanceled();
	};

		bool res = bt_->exportToWav(container, diag.getSampleRate(), diag.getLoopCount(), bar);
		if (res) {
			QFile fp(path);
			if (!fp.open(QIODevice::WriteOnly)) throw FileOutputError(FileIO::FileType::WAV);
			fp.write(container.getPointer(), container.size());
			fp.close();
			bar();

			config_.lock()->setWorkingDirectory(QFileInfo(path).dir().path().toStdString());
		}
	}
	catch (...) {
		QMessageBox::critical(this, tr("Error"), tr("Failed to export to wav file."));
	}

	stream_->start();
}

void MainWindow::on_actionVGM_triggered()
{
	VgmExportSettingsDialog diag;
	if (diag.exec() != QDialog::Accepted) return;
	GD3Tag tag = diag.getGD3Tag();

	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString path = QFileDialog::getSaveFileName(
					   this, tr("Export to vgm"),
					   QString("%1/%2.vgm").arg(dir.isEmpty() ? "." : dir, getModuleFileBaseName()),
					   "VGM file (*.vgm)");
	if (path.isNull()) return;
	if (!path.endsWith(".vgm")) path += ".vgm";	// For linux

	QProgressDialog progress(
				tr("Export to VGM"),
				tr("Cancel"),
				0,
				static_cast<int>(bt_->getAllStepCount(bt_->getCurrentSongNumber(), 1)) + 3
				);
	progress.setValue(0);
	progress.setWindowFlags(progress.windowFlags()
							& ~Qt::WindowContextHelpButtonHint
							& ~Qt::WindowCloseButtonHint);
	progress.show();

	bt_->stopPlaySong();
	lockControls(false);
	stream_->stop();

	try {
		BinaryContainer container;
		auto bar = [&progress]() -> bool {
				   QApplication::processEvents();
				   progress.setValue(progress.value() + 1);
				   return progress.wasCanceled();
	};

		bool res = bt_->exportToVgm(container, diag.getExportTarget(), diag.enabledGD3(), tag, bar);
		if (res) {
			QFile fp(path);
			if (!fp.open(QIODevice::WriteOnly)) throw FileOutputError(FileIO::FileType::VGM);
			fp.write(container.getPointer(), container.size());
			fp.close();
			bar();

			config_.lock()->setWorkingDirectory(QFileInfo(path).dir().path().toStdString());
		}
	}
	catch (...) {
		QMessageBox::critical(this, tr("Error"), tr("Failed to export to vgm file."));
	}

	stream_->start();
}

void MainWindow::on_actionS98_triggered()
{
	S98ExportSettingsDialog diag;
	if (diag.exec() != QDialog::Accepted) return;
	S98Tag tag = diag.getS98Tag();

	QString dir = QString::fromStdString(config_.lock()->getWorkingDirectory());
	QString path = QFileDialog::getSaveFileName(
					   this, tr("Export to s98"),
					   QString("%1/%2.s98").arg(dir.isEmpty() ? "." : dir, getModuleFileBaseName()),
					   "S98 file (*.s98)");
	if (path.isNull()) return;
	if (!path.endsWith(".s98")) path += ".s98";	// For linux

	QProgressDialog progress(
				tr("Export to S98"),
				tr("Cancel"),
				0,
				static_cast<int>(bt_->getAllStepCount(bt_->getCurrentSongNumber(), 1)) + 3
				);
	progress.setValue(0);
	progress.setWindowFlags(progress.windowFlags()
							& ~Qt::WindowContextHelpButtonHint
							& ~Qt::WindowCloseButtonHint);
	progress.show();

	bt_->stopPlaySong();
	lockControls(false);
	stream_->stop();

	try {
		BinaryContainer container;
		auto bar = [&progress]() -> bool {
				   QApplication::processEvents();
				   progress.setValue(progress.value() + 1);
				   return progress.wasCanceled();
	};

		bool res = bt_->exportToS98(container, diag.getExportTarget(), diag.enabledTag(),
									tag, diag.getResolution(), bar);
		if (res) {
			QFile fp(path);
			if (!fp.open(QIODevice::WriteOnly)) throw FileOutputError(FileIO::FileType::S98);
			fp.write(container.getPointer(), container.size());
			fp.close();
			bar();

			config_.lock()->setWorkingDirectory(QFileInfo(path).dir().path().toStdString());
		}
	}
	catch (...) {
		QMessageBox::critical(this, tr("Error"), tr("Failed to export to s98 file."));
	}

	stream_->start();
}

void MainWindow::on_actionMix_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onPasteMixPressed();
}

void MainWindow::on_actionOverwrite_triggered()
{
	if (isEditedPattern_) ui->patternEditor->onPasteOverwritePressed();
}

void MainWindow::onNewTickSignaledRealChip()
{
	onNewTickSignaled(bt_->streamCountUp());
}

void MainWindow::onNewTickSignaled(int state)
{
	if (!state) {	// New step
		int order = bt_->getPlayingOrderNumber();
		if (order > -1) {	// Playing
			ui->orderList->updatePositionByOrderUpdate(firstViewUpdateRequest_);
			ui->patternEditor->updatePositionByStepUpdate(firstViewUpdateRequest_);
			firstViewUpdateRequest_ = false;
			statusPlayPos_->setText(
						QString("%1/%2")
						.arg(order, 2, (config_.lock()->getShowRowNumberInHex() ? 16 : 10), QChar('0'))
						.arg(bt_->getPlayingStepNumber(), 2, 16, QChar('0')).toUpper());
		}
	}

	// Update BPM status
	if (bt_->getStreamGrooveEnabled()) {
		statusBpm_->setText("Groove");
	}
	else {
		// BPM = tempo * 6 / speed * 4 / 1st highlight
		double bpm = 24.0 * bt_->getStreamTempo() / bt_->getStreamSpeed() / highlight1_->value();
		statusBpm_->setText(QString::number(bpm, 'f', 2) + QString(" BPM"));
	}
}

void MainWindow::on_actionClear_triggered()
{
	fileHistory_->clearHistory();
	for (int i = ui->menu_Recent_Files->actions().count() - 1; 1 < i; --i)
		ui->menu_Recent_Files->removeAction(ui->menu_Recent_Files->actions().at(i));
}

void MainWindow::on_keyRepeatCheckBox_stateChanged(int arg1)
{
	config_.lock()->setKeyRepetition(arg1 == Qt::Checked);
}

void MainWindow::updateVisuals()
{
	int16_t wave[2 * OPNAController::OUTPUT_HISTORY_SIZE];
	bt_->getOutputHistory(wave);

	ui->waveVisual->setStereoSamples(wave, OPNAController::OUTPUT_HISTORY_SIZE);
}

void MainWindow::on_action_Effect_List_triggered()
{
	if (effListDiag_->isVisible()) effListDiag_->activateWindow();
	else effListDiag_->show();
}

void MainWindow::on_actionShortcuts_triggered()
{
	if (shortcutsDiag_->isVisible()) shortcutsDiag_->activateWindow();
	else shortcutsDiag_->show();
}

void MainWindow::on_actionExport_To_Bank_File_triggered()
{
	exportInstrumentsToBank();
}

void MainWindow::on_actionE_xpand_Effect_Column_triggered()
{
	ui->patternEditor->onExpandEffectColumn();
}

void MainWindow::on_actionS_hrink_Effect_Column_triggered()
{
	ui->patternEditor->onShrinkEffectColumn();
}

void MainWindow::on_actionRemove_Duplicate_Instruments_triggered()
{
	if (showUndoResetWarningDialog(tr("Do you want to remove all duplicate instruments?"))) {
		bt_->stopPlaySong();
		lockControls(false);

		std::vector<std::vector<int>> duplicates = bt_->checkDuplicateInstruments();
		auto list = ui->instrumentListWidget;
		for (auto& group : duplicates) {
			for (size_t i = 1; i < group.size(); ++i) {
				for (int j = 0; j < list->count(); ++j) {
					if (list->item(j)->data(Qt::UserRole).toInt() == group[i])
						removeInstrument(j);
				}
			}
		}
		bt_->replaceDuplicateInstrumentsInPatterns(duplicates);
		bt_->clearUnusedInstrumentProperties();
		bt_->clearCommandHistory();
		comStack_->clear();
		ui->patternEditor->onDuplicateInstrumentsRemoved();
		setModifiedTrue();
	}
}

void MainWindow::on_actionRename_Instrument_triggered()
{
	renameInstrument();
}
