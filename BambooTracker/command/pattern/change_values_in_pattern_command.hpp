/*
 * Copyright (C) 2020 Rerrah
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

#pragma once

#include <memory>
#include <vector>
#include "../abstract_command.hpp"
#include "module.hpp"

class ChangeValuesInPatternCommand : public AbstractCommand
{
public:
	ChangeValuesInPatternCommand(std::weak_ptr<Module> mod, int songNum,
								 int beginTrack, int beginColumn, int beginOrder, int beginStep,
								 int endTrack, int endColumn, int endStep, int value, bool isFMReversed);
	void redo() override;
	void undo() override;

private:
	std::weak_ptr<Module> mod_;
	int song_;
	int bTrack_, bCol_, order_, bStep_;
	int eTrack_, eCol_, eStep_;
	int diff_;
	bool fmReverse_;
	std::vector<std::vector<int>> prevVals_;
};
