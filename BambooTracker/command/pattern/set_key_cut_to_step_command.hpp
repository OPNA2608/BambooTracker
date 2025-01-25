/*
 * SPDX-FileCopyrightText: 2022 Rerrah
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <memory>
#include "../abstract_command.hpp"
#include "module.hpp"

class SetKeyCutToStepCommand final : public AbstractCommand
{
public:
	SetKeyCutToStepCommand(std::weak_ptr<Module> mod, int songNum, int trackNum,
						   int orderNum, int stepNum);
	bool redo() override;
	bool undo() override;

private:
	std::weak_ptr<Module> mod_;
	int song_, track_, order_, step_;
	int prevNote_, prevInst_, prevVol_;
	Step::PlainEffect prevEff_[Step::N_EFFECT];
};

