/*
 * SPDX-FileCopyrightText: 2018 Rerrah
 * SPDX-License-Identifier: MIT
 */

#include "order_list_panel.hpp"
#include <algorithm>
#include <vector>
#include <utility>
#include <thread>
#include <numeric>
#include <optional>
#include <QPainter>
#include <QFontMetrics>
#include <QFontInfo>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QAction>
#include <QPoint>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointF>
#endif
#include <QString>
#include <QIcon>
#include <QRegularExpression>
#include "playback.hpp"
#include "track.hpp"
#include "bamboo_tracker_defs.hpp"
#include "gui/dpi.hpp"
#include "gui/event_guard.hpp"
#include "gui/command/order/order_commands_qt.hpp"
#include "gui/command_result_message_box.hpp"
#include "gui/gui_utils.hpp"
#include "vector_2d.hpp"
#include "utils.hpp"

using Dpi::scaledQPixmap;
using Dpi::iRatio;
using Dpi::scaleRect;

OrderListPanel::OrderListPanel(QWidget *parent)
	: QWidget(parent),
	  config_(std::make_shared<Configuration>()),	// Dummy
	  rowFontWidth_(0),
	  rowFontHeight_(0),
	  rowFontAscent_(0),
	  rowFontLeading_(0),
	  headerFontAscent_(0),
	  widthSpace_(0),
	  rowNumWidthCnt_(0),
	  rowNumWidth_(0),
	  rowNumBase_(0),
	  trackWidth_(0),
	  columnsWidthFromLeftToEnd_(0),
	  headerHeight_(0),
	  curRowBaselineY_(0),
	  curRowY_(0),
	  visTracks_(1),	// Dummy
	  leftTrackVisIdx_(0),
	  curSongNum_(0),
	  curPos_{ 0, 0 },
	  hovPos_{ -1, -1 },
	  mousePressPos_{ -1, -1 },
	  mouseReleasePos_{ -1, -1 },
	  selLeftAbovePos_{ -1, -1 },
	  selRightBelowPos_{ -1, -1 },
	  shiftPressedPos_{ -1, -1 },
	  isIgnoreToSlider_(false),
	  isIgnoreToPattern_(false),
	  entryCnt_(0),
	  selectAllState_(-1),
	  viewedRowCnt_(1),
	  viewedRowsHeight_(0),
	  viewedRowOffset_(0),
	  viewedCenterY_(0),
	  viewedCenterBaseY_(0),
	  backChanged_(false),
	  textChanged_(false),
	  headerChanged_(false),
	  followModeChanged_(false),
	  hasFocussedBefore_(false),
	  orderDownCount_(0),
	  repaintable_(true),
	  repaintingCnt_(0),
	  playingRow_(-1),
	  insSc1_(Qt::Key_Insert, this, nullptr, nullptr, Qt::WidgetShortcut),
	  insSc2_(Qt::ALT | Qt::Key_B, this, nullptr, nullptr, Qt::WidgetShortcut),
	  menuSc_(Qt::Key_Menu, this, nullptr, nullptr, Qt::WidgetShortcut)
{
	setAttribute(Qt::WA_Hover);
	setFocusPolicy(Qt::ClickFocus);
	setContextMenuPolicy(Qt::CustomContextMenu);

	// Initialize font
	headerFontDef_ = [] {
		auto font = QApplication::font();
		font.setPointSize(10);
		return font;
	}();
	headerFont_ = headerFontDef_;

	rowFontDef_ = [] {
		QFont font("Monospace", 10);
		font.setStyleHint(QFont::TypeWriter);
		font.setStyleStrategy(QFont::PreferMatch);
		// Get actually used font
		QFontInfo info(font);
		return QFont(info.family(), info.pointSize());
	}();
	rowFont_ = rowFontDef_;

	updateSizes();

	// Track visibility
	songStyle_.type = SongType::Standard;	// Dummy
	songStyle_.trackAttribs.push_back({ 0, SoundSource::FM, 0 });	// Dummy
	std::iota(visTracks_.begin(), visTracks_.end(), 0);

	// Shortcuts
	QObject::connect(&insSc1_, &QShortcut::activated, this, &OrderListPanel::insertOrderBelow);
	QObject::connect(&insSc2_, &QShortcut::activated, this, &OrderListPanel::insertOrderBelow);
	QObject::connect(&menuSc_, &QShortcut::activated, this, [&] {
		showContextMenu(
					curPos_,
					QPoint(calculateColumnsWidthWithRowNum(leftTrackVisIdx_, curPos_.trackVisIdx), curRowY_ - 8));
	});
	onShortcutUpdated();
}

void OrderListPanel::setCore(std::shared_ptr<BambooTracker> core)
{
	bt_ = core;
}

void OrderListPanel::setCommandStack(std::weak_ptr<QUndoStack> stack)
{
	comStack_ = stack;
}

void OrderListPanel::setConfiguration(std::shared_ptr<Configuration> config)
{
	config_ = config;
}

void OrderListPanel::setColorPallete(std::shared_ptr<ColorPalette> palette)
{
	palette_ = palette;
}

void OrderListPanel::resetEntryCount()
{
	entryCnt_ = 0;
}

void OrderListPanel::waitPaintFinish()
{
	while (true) {
		if (repaintingCnt_.load())
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		else {
			curPos_.row = 0;	// Init
			return;
		}
	}
}

QFont OrderListPanel::getHeaderFont() const
{
	return headerFont_;
}

QFont OrderListPanel::getRowsFont() const
{
	return rowFont_;
}

QFont OrderListPanel::getDefaultHeaderFont() const
{
	return headerFontDef_;
}

QFont OrderListPanel::getDefaultRowsFont() const
{
	return rowFontDef_;
}

void OrderListPanel::setFonts(const QFont& headerFont, const QFont& rowsFont)
{
	headerFont_ = headerFont;
	rowFont_ = rowsFont;

	updateSizes();
	updateTracksWidthFromLeftToEnd();
	setMaximumWidth(calculateColumnsWidthWithRowNum(
						0, static_cast<int>(visTracks_.size()) - 1));

	redrawAll();
}

void OrderListPanel::setVisibleTracks(std::vector<int> tracks)
{
	visTracks_ = tracks;
	int max = static_cast<int>(tracks.size());
	bool cond = (max <= curPos_.trackVisIdx);
	if (cond) curPos_.trackVisIdx = max;
	leftTrackVisIdx_ = std::min(leftTrackVisIdx_, curPos_.trackVisIdx);
	updateTracksWidthFromLeftToEnd();
	setMaximumWidth(calculateColumnsWidthWithRowNum(
						0, static_cast<int>(visTracks_.size()) - 1));
	initDisplay();

	// Current track in core is changed in the pattern editor

	if (cond) {
		emit hScrollBarChangeRequested(config_->getMoveCursorByHorizontalScroll() ? curPos_.trackVisIdx
																				  : leftTrackVisIdx_);
	}

	redrawAll();
}

void OrderListPanel::updateSizes()
{
	QFontMetrics metrics(rowFont_);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
	rowFontWidth_ = metrics.horizontalAdvance('0');
#else
	rowFontWidth_ = metrics.width('0');
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
	rowFontAscent_ = metrics.capHeight();
#else
	rowFontAscent_ = metrics.boundingRect('X').height();
#endif
	rowFontLeading_ = metrics.ascent() - rowFontAscent_ + metrics.descent() / 2;
	rowFontHeight_ = rowFontAscent_ + rowFontLeading_;

	hdFontMets_ = std::make_unique<QFontMetrics>(headerFont_);
	headerHeight_ = hdFontMets_->height() + 5;
	headerFontAscent_ = hdFontMets_->ascent() + 2;

	/* Width & height */
	widthSpace_ = rowFontWidth_ / 4;
	trackWidth_ = rowFontWidth_ * 3 + widthSpace_ * 2;
	if (config_->getShowRowNumberInHex()) {
		rowNumWidthCnt_ = 2;
		rowNumBase_ = 16;
	}
	else {
		rowNumWidthCnt_ = 3;
		rowNumBase_ = 10;
	}
	rowNumWidth_ = rowFontWidth_ * rowNumWidthCnt_ + widthSpace_;

	initDisplay();
}

void OrderListPanel::initDisplay()
{
	int width = geometry().width();
	int ratio = iRatio(*this);

	// Recalculate pixmap sizes
	viewedRegionHeight_ = std::max((geometry().height() - headerHeight_), rowFontHeight_);
	int cnt = viewedRegionHeight_ / rowFontHeight_;
	viewedRowCnt_ = (cnt % 2) ? (cnt + 2) : (cnt + 1);
	viewedRowsHeight_ = viewedRowCnt_ * rowFontHeight_;

	viewedRowOffset_ = (viewedRowsHeight_ - viewedRegionHeight_) >> 1;
	viewedCenterY_ = (viewedRowsHeight_ - rowFontHeight_) >> 1;
	viewedCenterBaseY_ = viewedCenterY_ + rowFontAscent_ + (rowFontLeading_ >> 1);

	completePixmap_ = scaledQPixmap(geometry().size(), ratio);
	backPixmap_ = scaledQPixmap(width, viewedRowsHeight_, ratio);
	textPixmap_ = scaledQPixmap(width, viewedRowsHeight_, ratio);
	headerPixmap_ = scaledQPixmap(width, headerHeight_, ratio);
}

void OrderListPanel::drawList(const QRect &rect)
{
	if (repaintable_.load()) {
		repaintable_.store(false);
		++repaintingCnt_;	// Use module data after this line

		if (backChanged_ || textChanged_ || headerChanged_ || orderDownCount_ || followModeChanged_) {
			int ratio = iRatio(*this);

			int maxWidth = std::min(geometry().width(), columnsWidthFromLeftToEnd_);
			completePixmap_.fill(palette_->odrBackColor);

			if (orderDownCount_ && !followModeChanged_) {
				quickDrawRows(maxWidth);
			}
			else {
				backPixmap_.fill(Qt::transparent);
				if (textChanged_) textPixmap_.fill(Qt::transparent);
				drawRows(maxWidth);
			}
			drawBorders(maxWidth);

			if (headerChanged_) {
				// headerPixmap_->fill(Qt::transparent);
				drawHeaders(maxWidth);
			}

			{
				QPainter mergePainter(&completePixmap_);
				QRect rowsRect(0, viewedRowOffset_, maxWidth, viewedRegionHeight_);
				rowsRect = scaleRect(rowsRect, ratio);
				QRect inViewRect(0, headerHeight_, maxWidth, viewedRegionHeight_);
				mergePainter.drawPixmap(inViewRect, backPixmap_, rowsRect);
				mergePainter.drawPixmap(inViewRect, textPixmap_, rowsRect);
				mergePainter.drawPixmap(QPoint(0, 0), headerPixmap_);
			}

			if (!hasFocus()) drawShadow();

			backChanged_ = false;
			textChanged_ = false;
			headerChanged_ = false;
			followModeChanged_ = false;
			orderDownCount_ = 0;

		}

		--repaintingCnt_;	// Used module data until this line
		repaintable_.store(true);
	}

	QPainter completePainter(this);
	completePainter.drawPixmap(rect, completePixmap_);
}

void OrderListPanel::drawRows(int maxWidth)
{
	QPainter textPainter(&textPixmap_);
	QPainter backPainter(&backPixmap_);
	textPainter.setFont(rowFont_);

	std::vector<OrderInfo> orderRowData_;
	int textOffset = trackWidth_ / 2 - rowFontWidth_;

	/* Current row */
	// Fill row
	backPainter.fillRect(0, viewedCenterY_, maxWidth, rowFontHeight_,
						 hasFocus() ? palette_->odrCurEditRowColor : palette_->odrCurRowColor);
	if (textChanged_) {
		// Row number
		textPainter.setPen(palette_->odrRowNumColor);
		textPainter.drawText(1, viewedCenterBaseY_, QString("%1").arg(
								 curPos_.row, rowNumWidthCnt_, rowNumBase_, QChar('0')
								 ).toUpper());
	}
	// Order data
	orderRowData_ = bt_->getOrderData(curSongNum_, curPos_.row);
	textPainter.setPen(palette_->odrCurTextColor);
	for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
		if (trackVisIdx == curPos_.trackVisIdx)	// Paint current cell
			backPainter.fillRect(x, viewedCenterY_, trackWidth_, rowFontHeight_, palette_->odrCurCellColor);
		if (((hovPos_.row == curPos_.row || hovPos_.row == -2) && hovPos_.trackVisIdx == trackVisIdx)
				|| (hovPos_.trackVisIdx == -2 && hovPos_.row == curPos_.row))	// Paint hover
			backPainter.fillRect(x, viewedCenterY_, trackWidth_, rowFontHeight_, palette_->odrHovCellColor);
		if ((selLeftAbovePos_.trackVisIdx >= 0 && selLeftAbovePos_.row >= 0)
				&& isSelectedCell(trackVisIdx, curPos_.row))	// Paint selected
			backPainter.fillRect(x, viewedCenterY_, trackWidth_, rowFontHeight_, palette_->odrSelCellColor);
		if (textChanged_) {
			textPainter.drawText(
						x + textOffset,
						viewedCenterBaseY_,
						QString("%1")
						.arg(orderRowData_.at(static_cast<size_t>(visTracks_.at(trackVisIdx))).patten, 2, 16, QChar('0')).toUpper()
						);
		}

		x += trackWidth_;
	}
	viewedCenterPos_.row = curPos_.row;

	int rowNum;
	int rowY, baseY, endY;
	int playOdrNum = bt_->getPlayingOrderNumber();

	/* Previous rows */
	viewedFirstPos_.row = curPos_.row;
	endY = std::max(0, viewedCenterY_ - rowFontHeight_ * curPos_.row);
	for (rowY = viewedCenterY_ - rowFontHeight_, baseY = viewedCenterBaseY_ - rowFontHeight_, rowNum = curPos_.row - 1;
		 rowY >= endY;
		 rowY -= rowFontHeight_, baseY -= rowFontHeight_, --rowNum) {
		QColor rowColor;
		if (!config_->getFollowMode() && rowNum == playOdrNum) {
			rowColor = palette_->odrPlayRowColor;
		}
		else {
			rowColor = palette_->odrDefRowColor;
		}

		// Fill row
		backPainter.fillRect(0, rowY, maxWidth, rowFontHeight_, rowColor);
		if (textChanged_) {
			// Row number
			textPainter.setPen(palette_->odrRowNumColor);
			textPainter.drawText(1, baseY, QString("%1").arg(
									 rowNum, rowNumWidthCnt_, rowNumBase_, QChar('0')
									 ).toUpper());
		}
		// Order data
		orderRowData_ = bt_->getOrderData(curSongNum_, rowNum);
		textPainter.setPen(palette_->odrDefTextColor);
		for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
			if (((hovPos_.row == rowNum || hovPos_.row == -2) && hovPos_.trackVisIdx == trackVisIdx)
					|| (hovPos_.trackVisIdx == -2 && hovPos_.row == rowNum))	// Paint hover
				backPainter.fillRect(x, rowY, trackWidth_, rowFontHeight_, palette_->odrHovCellColor);
			if ((selLeftAbovePos_.trackVisIdx >= 0 && selLeftAbovePos_.row >= 0)
					&& isSelectedCell(trackVisIdx, rowNum))	// Paint selected
				backPainter.fillRect(x, rowY, trackWidth_, rowFontHeight_, palette_->odrSelCellColor);
			if (textChanged_) {
				textPainter.drawText(
							x + textOffset,
							baseY,
							QString("%1")
							.arg(orderRowData_.at(static_cast<size_t>(visTracks_.at(trackVisIdx))).patten, 2, 16, QChar('0')).toUpper()
							);
			}

			x += trackWidth_;
		}
		viewedFirstPos_.row = rowNum;
	}

	/* Next rows */
	viewedLastPos_.row = curPos_.row;
	endY = std::min(viewedRowsHeight_ - viewedRowOffset_,
					viewedCenterY_ + rowFontHeight_ * (static_cast<int>(bt_->getOrderSize(curSongNum_)) - curPos_.row - 1));
	for (rowY = viewedCenterY_ + rowFontHeight_, baseY = viewedCenterBaseY_ + rowFontHeight_, rowNum = curPos_.row + 1;
		 rowY <= endY;
		 rowY += rowFontHeight_, baseY += rowFontHeight_, ++rowNum) {
		QColor rowColor;
		if (!config_->getFollowMode() && rowNum == playOdrNum)
			rowColor = palette_->odrPlayRowColor;
		else
			rowColor = palette_->odrDefRowColor;

		// Fill row
		backPainter.fillRect(0, rowY, maxWidth, rowFontHeight_, rowColor);
		if (textChanged_) {
			// Row number
			textPainter.setPen(palette_->odrRowNumColor);
			textPainter.drawText(1, baseY, QString("%1").arg(
									 rowNum, rowNumWidthCnt_, rowNumBase_, QChar('0')
									 ).toUpper());
		}
		// Order data
		orderRowData_ = bt_->getOrderData(curSongNum_, rowNum);
		textPainter.setPen(palette_->odrDefTextColor);
		for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
			if (((hovPos_.row == rowNum || hovPos_.row == -2) && hovPos_.trackVisIdx == trackVisIdx)
					|| (hovPos_.trackVisIdx == -2 && hovPos_.row == rowNum))	// Paint hover
				backPainter.fillRect(x, rowY, trackWidth_, rowFontHeight_, palette_->odrHovCellColor);
			if ((selLeftAbovePos_.trackVisIdx >= 0 && selLeftAbovePos_.row >= 0)
					&& isSelectedCell(trackVisIdx, rowNum))	// Paint selected
				backPainter.fillRect(x, rowY, trackWidth_, rowFontHeight_, palette_->odrSelCellColor);
			if (textChanged_) {
				textPainter.drawText(
							x + textOffset,
							baseY,
							QString("%1")
							.arg(orderRowData_.at(static_cast<size_t>(visTracks_.at(trackVisIdx))).patten, 2, 16, QChar('0')).toUpper()
							);
			}

			x += trackWidth_;
		}
		viewedLastPos_.row = rowNum;
	}
}

void OrderListPanel::quickDrawRows(int maxWidth)
{
	int ratio = iRatio(*this);

	int halfRowsCnt = viewedRowCnt_ >> 1;
	int shift = rowFontHeight_ * orderDownCount_;

	/* Move up by */
	{
		// QPixmap::scroll() takes physical pixels, not virtual.
		int phShift = shift * ratio;
		QRect srcRect(0, 0, maxWidth, viewedRowsHeight_);
		srcRect = scaleRect(srcRect, ratio);

		textPixmap_.scroll(0, -phShift, srcRect);
		backPixmap_.scroll(0, -phShift, srcRect);
	}

	{
		int fpos = viewedCenterPos_.row + orderDownCount_ - halfRowsCnt;
		if (fpos >= 0) viewedFirstPos_.row = fpos;
	}

	QPainter textPainter(&textPixmap_);
	QPainter backPainter(&backPixmap_);
	textPainter.setFont(rowFont_);

	std::vector<OrderInfo> orderRowData_;
	int textOffset = trackWidth_ / 2 - rowFontWidth_;

	/* Clear previous cursor row, current cursor row and last rows text */
	int prevY = viewedCenterY_ - shift;
	int lastY = viewedRowsHeight_ - shift;
	textPainter.setCompositionMode(QPainter::CompositionMode_Source);
	textPainter.fillRect(0, prevY, maxWidth, rowFontHeight_, Qt::transparent);
	textPainter.fillRect(0, viewedCenterY_, maxWidth, rowFontHeight_, Qt::transparent);
	textPainter.fillRect(0, lastY, maxWidth, shift, Qt::transparent);
	textPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);

	/* Redraw previous cursor row */
	{
		int baseY = viewedCenterBaseY_ - shift;

		// Fill row
		backPainter.fillRect(0, prevY, maxWidth, rowFontHeight_, palette_->odrDefRowColor);
		// Row number
		textPainter.setPen(palette_->odrRowNumColor);
		textPainter.drawText(1, baseY, QString("%1").arg(
								 viewedCenterPos_.row, rowNumWidthCnt_, rowNumBase_, QChar('0')
								 ).toUpper());
		// Order data
		orderRowData_ = bt_->getOrderData(curSongNum_, viewedCenterPos_.row);
		textPainter.setPen(palette_->odrDefTextColor);
		for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
			if (((hovPos_.row == viewedCenterPos_.row || hovPos_.row == -2) && hovPos_.trackVisIdx == trackVisIdx)
					|| (hovPos_.trackVisIdx == -2 && hovPos_.row == viewedCenterPos_.row))	// Paint hover
				backPainter.fillRect(x, prevY, trackWidth_, rowFontHeight_, palette_->odrHovCellColor);
			if ((selLeftAbovePos_.trackVisIdx >= 0 && selLeftAbovePos_.row >= 0)
					&& isSelectedCell(trackVisIdx, viewedCenterPos_.row))	// Paint selected
				backPainter.fillRect(x, prevY, trackWidth_, rowFontHeight_, palette_->odrSelCellColor);
			textPainter.drawText(
						x + textOffset,
						baseY,
						QString("%1")
						.arg(orderRowData_.at(static_cast<size_t>(visTracks_.at(trackVisIdx))).patten, 2, 16, QChar('0')).toUpper()
						);

			x += trackWidth_;
		}
	}

	/* Redraw current cursor row */
	// Fill row
	backPainter.fillRect(0, viewedCenterY_, maxWidth, rowFontHeight_,
						 hasFocus() ? palette_->odrCurEditRowColor : palette_->odrCurRowColor);
	// Row number
	textPainter.setPen(palette_->odrRowNumColor);
	textPainter.drawText(1, viewedCenterBaseY_, QString("%1").arg(
							 curPos_.row, rowNumWidthCnt_, rowNumBase_, QChar('0')
							 ).toUpper());
	// Order data
	orderRowData_ = bt_->getOrderData(curSongNum_, curPos_.row);
	textPainter.setPen(palette_->odrCurTextColor);
	for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
		if (trackVisIdx == curPos_.trackVisIdx)	// Paint current cell
			backPainter.fillRect(x, viewedCenterY_, trackWidth_, rowFontHeight_, palette_->odrCurCellColor);
		if (((hovPos_.row == curPos_.row || hovPos_.row == -2) && hovPos_.trackVisIdx == trackVisIdx)
				|| (hovPos_.trackVisIdx == -2 && hovPos_.row == curPos_.row))	// Paint hover
			backPainter.fillRect(x, viewedCenterY_, trackWidth_, rowFontHeight_, palette_->odrHovCellColor);
		if ((selLeftAbovePos_.trackVisIdx >= 0 && selLeftAbovePos_.row >= 0)
				&& isSelectedCell(trackVisIdx, curPos_.row))	// Paint selected
			backPainter.fillRect(x, viewedCenterY_, trackWidth_, rowFontHeight_, palette_->odrSelCellColor);
		textPainter.drawText(
					x + textOffset,
					viewedCenterBaseY_,
					QString("%1")
					.arg(orderRowData_.at(static_cast<size_t>(visTracks_.at(trackVisIdx))).patten, 2, 16, QChar('0')).toUpper()
					);

		x += trackWidth_;
	}
	viewedCenterPos_ = curPos_;

	/* Draw new rows at last if necessary */
	{
		int bpos = viewedCenterPos_.row + halfRowsCnt;
		int last = static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1;
		bool needClear;
		if (bpos < last) {
			needClear = false;
			bpos = std::exchange(viewedLastPos_.row, bpos);
		}
		else {
			needClear = true;
			bpos = std::exchange(viewedLastPos_.row, last);
		}
		int baseY = lastY + (viewedCenterBaseY_ - viewedCenterY_);
		while (true) {
			if (bpos == viewedLastPos_.row) {
				if (needClear) {	// Clear row
					backPainter.setCompositionMode(QPainter::CompositionMode_Source);
					backPainter.fillRect(0, lastY, maxWidth, shift, Qt::transparent);
				}
				break;
			}
			++bpos;

			// Fill row
			backPainter.fillRect(0, lastY, maxWidth, rowFontHeight_, palette_->odrDefRowColor);
			// Row number
			textPainter.setPen(palette_->odrRowNumColor);
			textPainter.drawText(1, baseY, QString("%1").arg(
									 viewedLastPos_.row, rowNumWidthCnt_, rowNumBase_, QChar('0')
									 ).toUpper());
			// Order data
			orderRowData_ = bt_->getOrderData(curSongNum_, viewedLastPos_.row);
			textPainter.setPen(palette_->odrDefTextColor);
			for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
				if (((hovPos_.row == viewedLastPos_.row || hovPos_.row == -2) && hovPos_.trackVisIdx == trackVisIdx)
						|| (hovPos_.trackVisIdx == -2 && hovPos_.row == viewedLastPos_.row))	// Paint hover
					backPainter.fillRect(x, lastY, trackWidth_, rowFontHeight_, palette_->odrHovCellColor);
				if ((selLeftAbovePos_.trackVisIdx >= 0 && selLeftAbovePos_.row >= 0)
						&& isSelectedCell(trackVisIdx, viewedLastPos_.row))	// Paint selected
					backPainter.fillRect(x, lastY, trackWidth_, rowFontHeight_, palette_->odrSelCellColor);
				textPainter.drawText(
							x + textOffset,
							baseY,
							QString("%1")
							.arg(orderRowData_.at(static_cast<size_t>(visTracks_.at(trackVisIdx))).patten, 2, 16, QChar('0')).toUpper()
							);

				x += trackWidth_;
			}

			baseY += rowFontHeight_;
			lastY += rowFontHeight_;
		}
	}
}

void OrderListPanel::drawHeaders(int maxWidth)
{
	static const QString RHYTM_NAMES[6] = {
		"BD", "SD", "TOP", "HH", "TOM", "RIM"
	};
	QPainter painter(&headerPixmap_);
	painter.setFont(headerFont_);

	painter.fillRect(0, 0, geometry().width(), headerHeight_, palette_->odrHeaderRowColor);
	painter.setPen(palette_->odrHeaderBorderColor);
	qreal bottomLineY = headerHeight_ - 0.5;
	painter.drawLine(QPointF(0., bottomLineY), QPointF(geometry().width(), bottomLineY));

	for (int x = rowNumWidth_, trackVisIdx = leftTrackVisIdx_; x < maxWidth; ++trackVisIdx) {
		painter.setPen(palette_->odrHeaderBorderColor);
		painter.drawLine(x, 0, x, headerHeight_);
		QString str;
		auto& attrib = songStyle_.trackAttribs[static_cast<size_t>(visTracks_.at(trackVisIdx))];
		switch (attrib.source) {
		case SoundSource::FM:
			switch (songStyle_.type) {
			case SongType::Standard:
				str = "FM" + QString::number(attrib.channelInSource + 1);
				break;
			case SongType::FM3chExpanded:
				switch (attrib.channelInSource) {
				case 2:		str = "OP1";	break;
				case 6:		str = "OP2";	break;
				case 7:		str = "OP3";	break;
				case 8:		str = "OP4";	break;
				default:	str = "FM" + QString::number(attrib.channelInSource + 1);	break;
				}
				break;
			}
			break;
		case SoundSource::SSG:
			str = "SG" + QString::number(attrib.channelInSource + 1);
			break;
		case SoundSource::RHYTHM:
			str = RHYTM_NAMES[attrib.channelInSource];
			break;
		case SoundSource::ADPCM:
			str = "AP";
			break;
		}

		painter.setPen(palette_->odrHeaderTextColor);
		painter.drawText(QRectF(x, 0, trackWidth_, headerFontAscent_), Qt::AlignCenter, str);

		x += trackWidth_;
	}
}

void OrderListPanel::drawBorders(int maxWidth)
{
	QPainter painter(&backPixmap_);
	painter.setPen(palette_->odrBorderColor);
	painter.drawLine(rowNumWidth_, 0, rowNumWidth_, backPixmap_.height());
	for (int x = rowNumWidth_ + trackWidth_; x <= maxWidth; x += trackWidth_) {
		painter.drawLine(x, 0, x, backPixmap_.height());
	}
}

void OrderListPanel::drawShadow()
{
	QPainter painter(&completePixmap_);
	painter.fillRect(0, 0, geometry().width(), geometry().height(), palette_->odrUnfocusedShadowColor);
}

void OrderListPanel::moveCursorToRight(int n)
{
	int oldLeftTrackIdx = leftTrackVisIdx_;
	int prevTrackIdx = curPos_.trackVisIdx;
	int tmp = curPos_.trackVisIdx + n;
	if (n > 0) {
		while (true) {
			int sub = tmp - static_cast<int>(visTracks_.size());
			if (sub < 0) {
				curPos_.trackVisIdx = tmp;
				break;
			}
			else {
				if (config_->getWarpCursor()) {
					tmp = sub;
				}
				else {
					curPos_.trackVisIdx = static_cast<int>(visTracks_.size()) - 1;
					break;
				}
			}
		}
	}
	else {
		while (true) {
			int add = tmp + static_cast<int>(visTracks_.size());
			if (tmp < 0) {
				if (config_->getWarpCursor()) {
					tmp = add;
				}
				else {
					curPos_.trackVisIdx = 0;
					break;
				}
			}
			else {
				curPos_.trackVisIdx = tmp;
				break;
			}
		}
	}
	if (prevTrackIdx < curPos_.trackVisIdx) {
		while (calculateColumnsWidthWithRowNum(leftTrackVisIdx_, curPos_.trackVisIdx) > geometry().width())
			++leftTrackVisIdx_;
	}
	else {
		if (curPos_.trackVisIdx < leftTrackVisIdx_) leftTrackVisIdx_ = curPos_.trackVisIdx;
	}

	updateTracksWidthFromLeftToEnd();
	entryCnt_ = 0;

	if (!isIgnoreToSlider_) {	// Send to slider
		emit hScrollBarChangeRequested(config_->getMoveCursorByHorizontalScroll() ? curPos_.trackVisIdx
																				  : leftTrackVisIdx_);
	}

	if (!isIgnoreToPattern_) emit currentTrackChanged(curPos_.trackVisIdx);	// Send to pattern editor

	// Request fore-background repaint if leftmost track is changed else request only background repaint
	if (leftTrackVisIdx_ != oldLeftTrackIdx) {
		headerChanged_ = true;
		textChanged_ = true;
	}
	backChanged_ = true;
	repaint();
}

void OrderListPanel::moveViewToRight(int n)
{
	leftTrackVisIdx_ += n;
	updateTracksWidthFromLeftToEnd();

	// Move cursor and repaint all
	headerChanged_ = true;
	textChanged_ = true;
	moveCursorToRight(n);
}

void OrderListPanel::moveCursorToDown(int n)
{
	int tmp = curPos_.row + n;
	int endRow = static_cast<int>(bt_->getOrderSize(curSongNum_));
	if (n > 0) {
		while (true) {
			int sub = tmp - endRow;
			if (sub < 0) {
				curPos_.row = tmp;
				break;
			}
			else {
				tmp = sub;
			}
		}
	}
	else {
		while (true) {
			int add = tmp + endRow;
			if (tmp < 0) {
				tmp = add;
			}
			else {
				curPos_.row = tmp;
				break;
			}
		}
	}

	entryCnt_ = 0;

	if (!isIgnoreToSlider_)		// Send to slider
		emit vScrollBarChangeRequested(curPos_.row, static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1);

	if (!isIgnoreToPattern_)	// Send to pattern editor
		emit currentOrderChanged(curPos_.row);

	backChanged_ = true;
	textChanged_ = true;
	repaint();
}

void OrderListPanel::changeEditable()
{
	backChanged_ = true;
	repaint();
}

int OrderListPanel::getFullColumnSize() const
{
	return static_cast<int>(visTracks_.size()) - 1;
}

void OrderListPanel::updatePositionByOrderUpdate(bool isFirstUpdate, bool forceJump, bool trackChanged)
{	
	int prev = std::exchange(playingRow_, bt_->getPlayingOrderNumber());
	if (!forceJump && !config_->getFollowMode() && prev != playingRow_) {	// Repaint only background
		backChanged_ = true;
		repaint();
		return;
	}

	if (trackChanged) {	// Update horizontal position
		int trackVisIdx = std::distance(visTracks_.begin(), utils::find(visTracks_, bt_->getCurrentTrackAttribute().number));
		int prevTrackIdx = std::exchange(curPos_.trackVisIdx, trackVisIdx);
		if (prevTrackIdx < curPos_.trackVisIdx) {
			while (calculateColumnsWidthWithRowNum(leftTrackVisIdx_, curPos_.trackVisIdx) > geometry().width()) {
				++leftTrackVisIdx_;
				headerChanged_ = true;
			}
		}
		else {
			if (curPos_.trackVisIdx < leftTrackVisIdx_) {
				leftTrackVisIdx_ = curPos_.trackVisIdx;
				headerChanged_ = true;
			}
		}

		updateTracksWidthFromLeftToEnd();

		emit hScrollBarChangeRequested(config_->getMoveCursorByHorizontalScroll() ? curPos_.trackVisIdx
																				  : leftTrackVisIdx_);
	}

	int tmp = std::exchange(curPos_.row, bt_->getCurrentOrderNumber());
	int d = curPos_.row - tmp;
	if (d) {
		emit vScrollBarChangeRequested(curPos_.row, static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1);

		// Redraw entire area in first update and jumping order
		orderDownCount_ = (isFirstUpdate || d < 0 || (viewedRowCnt_ >> 1) < d) ? 0 : d;
	}
	else if (!trackChanged) return;

	entryCnt_ = 0;
	textChanged_ = true;
	backChanged_ = true;
	repaint();
}

int OrderListPanel::getScrollableCountByTrack() const
{
	int width = rowNumWidth_;
	size_t i = visTracks_.size();
	do {
		--i;
		width += trackWidth_;
		if (geometry().width() < width) {
			return static_cast<int>(i + 1);
		}
	} while (i);
	return 0;
}

void OrderListPanel::redrawByPatternChanged(bool ordersLengthChanged)
{
	textChanged_ = true;

	// When length of orders is changed, redraw all area
	if (ordersLengthChanged) backChanged_ = true;

	repaint();
}

void OrderListPanel::redrawByFocusChanged()
{
	if (hasFocussedBefore_) {
		backChanged_ = true;
		repaint();
	}
	else {
		redrawAll();
		hasFocussedBefore_ = true;
	}
}

void OrderListPanel::redrawByHoverChanged()
{
	headerChanged_ = true;
	backChanged_ = true;
	repaint();
}

void OrderListPanel::redrawAll()
{
	backChanged_ = true;
	textChanged_ = true;
	headerChanged_ = true;
	orderDownCount_ = 0;	// Prevent quick draw
	repaint();
}

bool OrderListPanel::enterOrder(int key)
{
	switch (key) {
	case Qt::Key_0:	setCellOrderNum(0x0);	return true;
	case Qt::Key_1:	setCellOrderNum(0x1);	return true;
	case Qt::Key_2:	setCellOrderNum(0x2);	return true;
	case Qt::Key_3:	setCellOrderNum(0x3);	return true;
	case Qt::Key_4:	setCellOrderNum(0x4);	return true;
	case Qt::Key_5:	setCellOrderNum(0x5);	return true;
	case Qt::Key_6:	setCellOrderNum(0x6);	return true;
	case Qt::Key_7:	setCellOrderNum(0x7);	return true;
	case Qt::Key_8:	setCellOrderNum(0x8);	return true;
	case Qt::Key_9:	setCellOrderNum(0x9);	return true;
	case Qt::Key_A:	setCellOrderNum(0xa);	return true;
	case Qt::Key_B:	setCellOrderNum(0xb);	return true;
	case Qt::Key_C:	setCellOrderNum(0xc);	return true;
	case Qt::Key_D:	setCellOrderNum(0xd);	return true;
	case Qt::Key_E:	setCellOrderNum(0xe);	return true;
	case Qt::Key_F:	setCellOrderNum(0xf);	return true;
	default:	return false;
	}
}

void OrderListPanel::setCellOrderNum(int n)
{
	bt_->setOrderPatternDigit(curSongNum_, visTracks_.at(curPos_.trackVisIdx), curPos_.row, n, (entryCnt_ == 1));
	comStack_.lock()->push(new SetPatternToOrderQtCommand(this, curPos_, (entryCnt_ == 1)));

	entryCnt_ = (entryCnt_ + 1) % 2;
	if ((!bt_->isPlaySong() || !bt_->isFollowPlay()) && !entryCnt_) moveCursorToDown(1);
}

void OrderListPanel::insertOrderBelow()
{
	if (!bt_->canAddNewOrder(curSongNum_)) return;

	bt_->insertOrderBelow(curSongNum_, curPos_.row);
	comStack_.lock()->push(new InsertOrderBelowQtCommand(this));
}

void OrderListPanel::deleteOrder()
{
	if (bt_->getOrderSize(curSongNum_) > 1) {
		bt_->deleteOrder(curSongNum_, curPos_.row);
		comStack_.lock()->push(new DeleteOrderQtCommand(this));
	}
}

void OrderListPanel::copySelectedCells()
{
	if (selLeftAbovePos_.row == -1) return;

	// Real selected region width
	int w = visTracks_.at(selRightBelowPos_.trackVisIdx) - visTracks_.at(selLeftAbovePos_.trackVisIdx) + 1;
	int h = selRightBelowPos_.row - selLeftAbovePos_.row + 1;

	QString str = QString("ORDER_COPY:%1,%2,")
				  .arg(QString::number(w), QString::number(h));
	for (int i = 0; i < h; ++i) {
		std::vector<OrderInfo> odrs = bt_->getOrderData(curSongNum_, selLeftAbovePos_.row + i);
		for (int j = 0; j < w; ++j) {
			str += QString::number(odrs.at(static_cast<size_t>(visTracks_.at(selLeftAbovePos_.trackVisIdx) + j)).patten);
			if (i < h - 1 || j < w - 1) str += ",";
		}
	}

	QApplication::clipboard()->setText(str);
}

void OrderListPanel::pasteCopiedCells(const OrderPosition& startPos)
{
	bool result = [&] {
		// Analyze text in clopboard.
		static const QRegularExpression COMMAND_REGEX {
			R"(^ORDER_COPY:(?<width>\d+),(?<height>\d+),(?<data>.+)$)"
		};
		const auto match = COMMAND_REGEX.match(QApplication::clipboard()->text());
		if (!match.hasMatch()) return false;

		std::size_t w = match.captured("width").toUInt();
		std::size_t h = match.captured("height").toUInt();
		if (w == 0 || h == 0) return false;

		QStringList data = match.captured("data").split(",");
		auto unmodifiedSize = data.size();
		data.removeAll("");
		if (static_cast<std::size_t>(data.size()) != w * h || data.size() != unmodifiedSize) {
			return false;
		}

		// Parse text.
		Vector2d<int> cells(h, w);
		for (std::size_t i = 0; i < h; ++i) {
			for (std::size_t j = 0; j < w; ++j) {
				bool isOk{};
				cells[i][j] = data[i * w + j].toInt(&isOk);
				if (!isOk) return false;
			}
		}

		// Send cells data.
		if (!bt_->pasteOrderCells(curSongNum_, visTracks_.at(startPos.trackVisIdx), startPos.row, cells)) {
			return false;
		}
		comStack_.lock()->push(new PasteCopiedDataToOrderQtCommand(this));

		return true;
	}();

	if (!result) command_result_message_box::showCommandInvokingErrorMessageBox(window());
}

void OrderListPanel::clonePatterns(const OrderPosition& singlePos)
{
	int bo, bt, eo, et;
	if (selLeftAbovePos_.row != -1) {
		bo = selLeftAbovePos_.row;
		bt = visTracks_.at(selLeftAbovePos_.trackVisIdx);
		eo = selRightBelowPos_.row;
		et = visTracks_.at(selRightBelowPos_.trackVisIdx);
	}
	else if (singlePos.row >= 0 && singlePos.trackVisIdx >= 0) {
		bo = eo = singlePos.row;
		bt = et = visTracks_.at(singlePos.trackVisIdx);
	}
	else return;

	bt_->clonePatterns(curSongNum_, bo, bt, eo, et);
	comStack_.lock()->push(new ClonePatternsQtCommand(this));
}

void OrderListPanel::setSelectedRectangle(const OrderPosition& start, const OrderPosition& end)
{
	if (start.trackVisIdx > end.trackVisIdx) {
		if (start.row > end.row) {
			selLeftAbovePos_ = end;
			selRightBelowPos_ = start;
		}
		else {
			selLeftAbovePos_ = { end.trackVisIdx, start.row };
			selRightBelowPos_ = { start.trackVisIdx, end.row };
		}
	}
	else {
		if (start.row > end.row) {
			selLeftAbovePos_ = { start.trackVisIdx, end.row };
			selRightBelowPos_ = { end.trackVisIdx, start.row };
		}
		else {
			selLeftAbovePos_ = start;
			selRightBelowPos_ = end;
		}
	}

	emit selected(true);

	backChanged_ = true;
	repaint();
}

bool OrderListPanel::isSelectedCell(int trackIdx, int row)
{
	return (selLeftAbovePos_.trackVisIdx <= trackIdx && selRightBelowPos_.trackVisIdx >= trackIdx
			&& selLeftAbovePos_.row <= row && selRightBelowPos_.row >= row);
}

void OrderListPanel::showContextMenu(const OrderPosition& pos, const QPoint& point)
{
	QMenu menu;
	// Leave Before Qt5.7.0 style due to windows xp
	QAction* insert = menu.addAction(tr("&Insert Order"));
	insert->setIcon(QIcon(":/icon/insert_order"));
	QObject::connect(insert, &QAction::triggered, this, [&] { insertOrderBelow(); });
	QAction* remove = menu.addAction(tr("&Remove Order"));
	remove->setIcon(QIcon(":/icon/remove_order"));
	QObject::connect(remove, &QAction::triggered, this, [&] { deleteOrder(); });
	QAction* duplicate = menu.addAction(tr("&Duplicate Order"));
	duplicate->setIcon(QIcon(":/icon/duplicate_order"));
	QObject::connect(duplicate, &QAction::triggered, this, &OrderListPanel::onDuplicatePressed);
	QAction* clonep = menu.addAction(tr("&Clone Patterns"));
	QAction::connect(clonep, &QAction::triggered, this, [&, pos] { clonePatterns(pos); });
	QAction* cloneo = menu.addAction(tr("Clone &Order"));
	QObject::connect(cloneo, &QAction::triggered, this, &OrderListPanel::onCloneOrderPressed);
	menu.addSeparator();
	QAction* moveUp = menu.addAction(tr("Move Order &Up"));
	moveUp->setIcon(QIcon(":/icon/order_up"));
	QObject::connect(moveUp, &QAction::triggered, this, [&] { onMoveOrderPressed(true); });
	QAction* moveDown = menu.addAction(tr("Move Order Do&wn"));
	moveDown->setIcon(QIcon(":/icon/order_down"));
	QObject::connect(moveDown, &QAction::triggered, this, [&] { onMoveOrderPressed(false); });
	menu.addSeparator();
	QAction* copy = menu.addAction(tr("Cop&y"));
	copy->setIcon(QIcon(":/icon/copy"));
	QObject::connect(copy, &QAction::triggered, this, &OrderListPanel::copySelectedCells);
	QAction* paste = menu.addAction(tr("&Paste"));
	paste->setIcon(QIcon(":/icon/paste"));
	QObject::connect(paste, &QAction::triggered, this, [&] { pasteCopiedCells(pos); });
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
	duplicate->setShortcutVisibleInContextMenu(true);
	clonep->setShortcutVisibleInContextMenu(true);
	copy->setShortcutVisibleInContextMenu(true);
	paste->setShortcutVisibleInContextMenu(true);
#endif
	auto shortcuts = config_->getShortcuts();
	duplicate->setShortcut(gui_utils::strToKeySeq(shortcuts.at(Configuration::ShortcutAction::DuplicateOrder)));
	clonep->setShortcut(gui_utils::strToKeySeq(shortcuts.at(Configuration::ShortcutAction::ClonePatterns)));
	cloneo->setShortcut(gui_utils::strToKeySeq(shortcuts.at(Configuration::ShortcutAction::CloneOrder)));
	copy->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_C));
	paste->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_V));

	bool notCurHov = (pos.row < 0 || pos.trackVisIdx < 0);
	if (notCurHov) {
		remove->setEnabled(false);
		moveUp->setEnabled(false);
		moveDown->setEnabled(false);
		copy->setEnabled(false);
		paste->setEnabled(false);
	}
	if (!bt_->canAddNewOrder(curSongNum_)) {
		insert->setEnabled(false);
		duplicate->setEnabled(false);
		moveUp->setEnabled(false);
		moveDown->setEnabled(false);
		copy->setEnabled(false);
		paste->setEnabled(false);
	}
	QString clipText = QApplication::clipboard()->text();
	if (!clipText.startsWith("ORDER_COPY")) {
		paste->setEnabled(false);
	}
	if (bt_->getOrderSize(curSongNum_) == 1) {
		remove->setEnabled(false);
	}
	if (selRightBelowPos_.row < 0
			|| !isSelectedCell(pos.trackVisIdx, pos.row)) {
		copy->setEnabled(false);
		// Turn off when no pattern is hilighted
		if (notCurHov) clonep->setEnabled(false);
	}
	if (pos.row == 0) {
		moveUp->setEnabled(false);
	}
	if (pos.row == static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1) {
		moveDown->setEnabled(false);
	}

	menu.exec(mapToGlobal(point));
}

/********** Slots **********/
void OrderListPanel::onHScrollBarChanged(int num)
{
	Ui::EventGuard eg(isIgnoreToSlider_);

	// Skip if position has already changed in panel
	if (config_->getMoveCursorByHorizontalScroll()) {
		if (int dif = num - curPos_.trackVisIdx) moveCursorToRight(dif);
	}
	else {
		if (int dif = num - leftTrackVisIdx_) moveViewToRight(dif);
	}
}

void OrderListPanel::onVScrollBarChanged(int num) {
	Ui::EventGuard eg(isIgnoreToSlider_);

	// Skip if position has already changed in panel
	if (int dif = num - curPos_.row) moveCursorToDown(dif);
}

void OrderListPanel::onPatternEditorCurrentTrackChanged(int idx)
{
	Ui::EventGuard eg(isIgnoreToPattern_);

	// Skip if position has already changed in panel
	if (int dif = idx - curPos_.trackVisIdx) moveCursorToRight(dif);
}

void OrderListPanel::onPatternEditorCurrentOrderChanged(int num) {
	Ui::EventGuard eg(isIgnoreToPattern_);

	// Skip if position has already changed in panel
	if (int dif = num - curPos_.row) moveCursorToDown(dif);
}

void OrderListPanel::onOrderEdited()
{
	// Move cursor
	int s = static_cast<int>(bt_->getOrderSize(curSongNum_));
	if (s <= curPos_.row) {
		curPos_.row = s - 1;
		bt_->setCurrentOrderNumber(curPos_.row);
	}

	emit orderEdited();
}

void OrderListPanel::onSongLoaded()
{
	curSongNum_ = bt_->getCurrentSongNumber();
	SongType prevType = songStyle_.type;
	songStyle_ = bt_->getSongStyle(curSongNum_);
	visTracks_ = gui_utils::adaptVisibleTrackList(visTracks_, prevType, songStyle_.type);
	curPos_ = { visTracks_.front(), bt_->getCurrentOrderNumber() };
	// Set cursor to the most lest-placed visible track
	if (visTracks_.front() != bt_->getCurrentTrackAttribute().number) {
		bt_->setCurrentTrack(visTracks_.front());
	}
	leftTrackVisIdx_ = 0;
	updateTracksWidthFromLeftToEnd();
	setMaximumWidth(columnsWidthFromLeftToEnd_);
	initDisplay();	// Call because resize event is not called during loading song

	hovPos_ = { -1, -1 };
	mousePressPos_ = { -1, -1 };
	mouseReleasePos_ = { -1, -1 };
	selLeftAbovePos_ = { -1, -1 };
	selRightBelowPos_ = { -1, -1 };
	shiftPressedPos_ = { -1, -1 };
	entryCnt_ = 0;
	selectAllState_ = -1;
	emit selected(false);

	redrawAll();
}

void OrderListPanel::onShortcutUpdated()
{
}

void OrderListPanel::onPastePressed()
{
	pasteCopiedCells(curPos_);
}

void OrderListPanel::onSelectPressed(int type)
{
	switch (type) {
	case 0:	// None
	{
		selLeftAbovePos_ = { -1, -1 };
		selRightBelowPos_ = { -1, -1 };
		selectAllState_ = -1;
		emit selected(false);
		backChanged_ = true;
		repaint();
		break;
	}
	case 1:	// All
	{
		int max = static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1;
		selectAllState_ = (selectAllState_ + 1) % 2;
		OrderPosition start, end;
		if (selectAllState_) {
			start = { 0, 0 };
			end = { static_cast<int>(visTracks_.size() - 1), max };
		}
		else {
			start = { curPos_.trackVisIdx, 0 };
			end = { curPos_.trackVisIdx, max };
		}
		setSelectedRectangle(start, end);
		break;
	}
	case 2:	// Row
	{
		selectAllState_ = -1;
		OrderPosition start = { 0, curPos_.row };
		OrderPosition end = { static_cast<int>(visTracks_.size() - 1), curPos_.row };
		setSelectedRectangle(start, end);
		break;
	}
	case 3:	// Column
	{
		selectAllState_ = -1;
		OrderPosition start = { curPos_.trackVisIdx, 0 };
		OrderPosition end = { curPos_.trackVisIdx, static_cast<int>(bt_->getOrderSize(curSongNum_) - 1) };
		setSelectedRectangle(start, end);
		break;
	}
	case 4:	// Pattern
	{
		selectAllState_ = -1;
		setSelectedRectangle(curPos_, curPos_);
		break;
	}
	case 5:	// Order
	{
		onSelectPressed(2);
		break;
	}
	}
}

void OrderListPanel::onDuplicatePressed()
{
	bt_->duplicateOrder(curSongNum_, curPos_.row);
	comStack_.lock()->push(new DuplicateOrderQtCommand(this));
}

void OrderListPanel::onMoveOrderPressed(bool isUp)
{
	if ((isUp && curPos_.row == 0)
			|| (!isUp && curPos_.row == static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1))
		return;

	bt_->MoveOrder(curSongNum_, curPos_.row, isUp);
	comStack_.lock()->push(new MoveOrderQtCommand(this));
}

void OrderListPanel::onClonePatternsPressed()
{
	clonePatterns(curPos_);
}

void OrderListPanel::onCloneOrderPressed()
{
	bt_->cloneOrder(curSongNum_, curPos_.row);
	comStack_.lock()->push(new CloneOrderQtCommand(this));
}

void OrderListPanel::onFollowModeChanged()
{
	curPos_.row = bt_->getCurrentOrderNumber();
	emit vScrollBarChangeRequested(curPos_.row, static_cast<int>(bt_->getOrderSize(curSongNum_)) - 1);

	// Force redraw all area
	followModeChanged_ = true;
	textChanged_ = true;
	backChanged_ = true;
	repaint();
}

void OrderListPanel::onStoppedPlaySong()
{
	followModeChanged_ = true;
	textChanged_ = true;
	backChanged_ = true;
	repaint();
}

void OrderListPanel::onGoOrderRequested(bool toNext)
{
	moveCursorToDown(toNext ? 1 : -1);
}

/********** Events **********/
bool OrderListPanel::event(QEvent* event)
{
	switch (event->type()) {
	case QEvent::KeyPress:
		return keyPressed(dynamic_cast<QKeyEvent*>(event));
	case QEvent::KeyRelease:
		return keyReleased(dynamic_cast<QKeyEvent*>(event));
	case QEvent::HoverMove:
		return mouseHoverd(dynamic_cast<QHoverEvent*>(event));
	default:
		return QWidget::event(event);
	}
}

bool OrderListPanel::keyPressed(QKeyEvent* event)
{	
	/* General Keys */
	switch (event->key()) {
	case Qt::Key_Shift:
		shiftPressedPos_ = curPos_;
		return true;
	case Qt::Key_Left:
		moveCursorToRight(-1);
		if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
		else onSelectPressed(0);
		return true;
	case Qt::Key_Right:
		moveCursorToRight(1);
		if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
		else onSelectPressed(0);
		return true;
	case Qt::Key_Up:
		if (bt_->isPlaySong() && bt_->isFollowPlay()) {
			return false;
		}
		else {
			moveCursorToDown(-1);
			if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
			else onSelectPressed(0);
			return true;
		}
	case Qt::Key_Down:
		if (bt_->isPlaySong() && bt_->isFollowPlay()) {
			return false;
		}
		else {
			moveCursorToDown(1);
			if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
			else onSelectPressed(0);
			return true;
		}
	case Qt::Key_Home:
		if (bt_->isPlaySong() && bt_->isFollowPlay()) {
			return false;
		}
		else {
			moveCursorToDown(-curPos_.row);
			if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
			else onSelectPressed(0);
			return true;
		}
	case Qt::Key_End:
		if (bt_->isPlaySong() && bt_->isFollowPlay()) {
			return false;
		}
		else {
			moveCursorToDown(
						static_cast<int>(bt_->getOrderSize(curSongNum_)) - curPos_.row - 1);
			if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
			else onSelectPressed(0);
			return true;
		}
	case Qt::Key_PageUp:
		if (bt_->isPlaySong() && bt_->isFollowPlay()) {
			return false;
		}
		else {
			moveCursorToDown(-static_cast<int>(config_->getPageJumpLength()));
			if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
			else onSelectPressed(0);
			return true;
		}
	case Qt::Key_PageDown:
		if (bt_->isPlaySong() && bt_->isFollowPlay()) {
			return false;
		}
		else {
			moveCursorToDown(static_cast<int>(config_->getPageJumpLength()));
			if (event->modifiers().testFlag(Qt::ShiftModifier)) setSelectedRectangle(shiftPressedPos_, curPos_);
			else onSelectPressed(0);
			return true;
		}
	default:
	{
		auto modifiers = event->modifiers();
		if (modifiers.testFlag(Qt::NoModifier) || modifiers.testFlag(Qt::KeypadModifier)) {
			return enterOrder(event->key());
		}
		return false;
	}
	}
}

bool OrderListPanel::keyReleased(QKeyEvent* event)
{
	switch (event->key()) {
	case Qt::Key_Shift:
		shiftPressedPos_ = { -1, -1 };
		return true;
	default:
		return false;
	}
}

void OrderListPanel::paintEvent(QPaintEvent* event)
{
	if (bt_) {
		const QRect& area = event->rect();
		if (area.x() == 0 && area.y() == 0) {
			drawList(area);
		}
		else {
			drawList(rect());
		}
	}
}

void OrderListPanel::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);

	// Recalculate center row position
	curRowBaselineY_ = (geometry().height() + headerHeight_) / 2;
	curRowY_ = curRowBaselineY_ + rowFontLeading_ / 2 - rowFontAscent_;

	initDisplay();

	redrawAll();
}

void OrderListPanel::mousePressEvent(QMouseEvent* event)
{
	mousePressPos_ = hovPos_;
	mouseReleasePos_ = { -1, -1 };

	if (event->button() == Qt::LeftButton) {
		selLeftAbovePos_ = { -1, -1 };
		selRightBelowPos_ = { -1, -1 };
		selectAllState_ = -1;
		emit selected(false);
	}
}

void OrderListPanel::mouseMoveEvent(QMouseEvent* event)
{
	if (event->buttons() & Qt::LeftButton) {
		if (mousePressPos_.trackVisIdx < 0 || mousePressPos_.row < 0) return;	// Start point is out of range

		if (hovPos_.trackVisIdx >= 0) {
			setSelectedRectangle(mousePressPos_, hovPos_);
		}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		QPointF pos = event->position();
#else
		QPoint pos = event->pos();
#endif
		if (pos.x() < rowNumWidth_ && leftTrackVisIdx_ > 0) {
			if (config_->getMoveCursorByHorizontalScroll())
				moveCursorToRight(-1);
			else
				moveViewToRight(-1);
		}
		else if (pos.x() > geometry().width() - rowNumWidth_ && hovPos_.trackVisIdx != -1) {
			if (config_->getMoveCursorByHorizontalScroll())
				moveCursorToRight(1);
			else
				moveViewToRight(1);
		}
		if (pos.y() < headerHeight_ + rowFontHeight_) {
			if (!bt_->isPlaySong() || !bt_->isFollowPlay()) moveCursorToDown(-1);
		}
		else if (pos.y() > geometry().height() - rowFontHeight_) {
			if (!bt_->isPlaySong() || !bt_->isFollowPlay()) moveCursorToDown(1);
		}
	}
}

void OrderListPanel::mouseReleaseEvent(QMouseEvent* event)
{
	mouseReleasePos_ = hovPos_;

	switch (event->button()) {
	case Qt::LeftButton:
		if (mousePressPos_ == mouseReleasePos_) {	// Jump cell
			if (hovPos_.row >= 0 && hovPos_.trackVisIdx >= 0) {
				int horDif = hovPos_.trackVisIdx - curPos_.trackVisIdx;
				int verDif = hovPos_.row - curPos_.row;
				moveCursorToRight(horDif);
				if (!bt_->isPlaySong() || !bt_->isFollowPlay()) moveCursorToDown(verDif);
			}
			else if (hovPos_.row == -2 && hovPos_.trackVisIdx >= 0) {	// Header
				int horDif = hovPos_.trackVisIdx - curPos_.trackVisIdx;
				moveCursorToRight(horDif);
			}
			else if (hovPos_.trackVisIdx == -2 && hovPos_.row >= 0) {	// Row number
				if (!bt_->isPlaySong() || !bt_->isFollowPlay()) {
					int verDif = hovPos_.row - curPos_.row;
					moveCursorToDown(verDif);
				}
			}
		}
		break;

	case Qt::RightButton:
		showContextMenu(mousePressPos_, event->pos());
		break;

	case Qt::XButton1:
		if (!bt_->isPlaySong() || !bt_->isFollowPlay()) {
			moveCursorToDown(-1);
		}
		break;

	case Qt::XButton2:
		if (!bt_->isPlaySong() || !bt_->isFollowPlay()) {
			moveCursorToDown(1);
		}
		break;

	default:
		break;
	}

	mousePressPos_ = { -1, -1 };
	mouseReleasePos_ = { -1, -1 };
}

bool OrderListPanel::mouseHoverd(QHoverEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF pos = event->position();
#else
	QPoint pos = event->pos();
#endif

	OrderPosition oldPos = hovPos_;

	// Detect row
	if (pos.y() <= headerHeight_) {
		hovPos_.row = -2;	// Header
	}
	else {
		if (pos.y() < curRowY_) {
			int tmp = curPos_.row + static_cast<int>(std::ceil((pos.y() - curRowY_) / rowFontHeight_)) - 1;
			hovPos_.row = (tmp < 0) ? -1 : tmp;
		}
		else {
			hovPos_.row = curPos_.row + static_cast<int>(std::floor((pos.y() - curRowY_) / rowFontHeight_));
			if (hovPos_.row >= static_cast<int>(bt_->getOrderSize(curSongNum_))) hovPos_.row = -1;
		}
	}

	// Detect track
	if (pos.x() <= rowNumWidth_) {
		hovPos_.trackVisIdx = -2;	// Row number
	}
	else {
		int tmpWidth = rowNumWidth_;
		for (int i = leftTrackVisIdx_; ; ) {
			tmpWidth += trackWidth_;
			if (pos.x() <= tmpWidth) {
				hovPos_.trackVisIdx = i;
				break;
			}
			++i;

			if (i == static_cast<int>(visTracks_.size())) {
				hovPos_.trackVisIdx = -1;
				break;
			}
		}
	}

	if (hovPos_ != oldPos) redrawByHoverChanged();

	return true;
}

void OrderListPanel::wheelEvent(QWheelEvent *event)
{
	if (bt_->isPlaySong() && bt_->isFollowPlay()) return;
	int degree = event->angleDelta().y() / 8;
	moveCursorToDown(-degree / 15);
}

void OrderListPanel::leaveEvent(QEvent*)
{
	// Clear mouse hover selection
	hovPos_ = { -1, -1 };
}
