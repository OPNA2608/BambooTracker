/*
 * Copyright (C) 2019 Rerrah
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "file_history_handler.hpp"
#include <QSettings>

// config path (*nix): ~/.config/<ORGANIZATION_>/<FILE_>.ini
const QString FileHistoryHandler::ORGANIZATION_ = "BambooTracker";
const QString FileHistoryHandler::FILE_ = "FileHistory";

FileHistoryHandler::FileHistoryHandler() {}

bool FileHistoryHandler::saveFileHistory(std::weak_ptr<FileHistory> history)
{
	try {
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, ORGANIZATION_, FILE_);
		settings.beginWriteArray("fileHistory");
		int n = 0;
		for (size_t i = 0; i < history.lock()->size(); ++i) {
			settings.setArrayIndex(n++);
			settings.setValue("path", history.lock()->at(i));
		}
		settings.endArray();
		return true;
	} catch (...) {
		return false;
	}
}

bool FileHistoryHandler::loadFileHistory(std::weak_ptr<FileHistory> history)
{
	try {
		QSettings settings(QSettings::IniFormat, QSettings::UserScope, ORGANIZATION_, FILE_);
		int size = settings.beginReadArray("fileHistory");
		history.lock()->clearHistory();
		for (int i = size - 1; 0 <= i; --i) {
			settings.setArrayIndex(i);
			history.lock()->addFile(settings.value("path").toString());
		}
		settings.endArray();
		return true;
	} catch (...) {
		return false;
	}
}
