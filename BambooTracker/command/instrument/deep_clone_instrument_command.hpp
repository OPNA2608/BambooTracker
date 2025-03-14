/*
 * SPDX-FileCopyrightText: 2018 Rerrah
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>
#include "../abstract_command.hpp"
#include "instruments_manager.hpp"

class DeepCloneInstrumentCommand final : public AbstractCommand
{
public:
	DeepCloneInstrumentCommand(std::weak_ptr<InstrumentsManager> manager, int num, int refNum);
	bool redo() override;
	bool undo() override;

private:
	std::weak_ptr<InstrumentsManager> manager_;
	int cloneInstNum_, refInstNum_;
};
