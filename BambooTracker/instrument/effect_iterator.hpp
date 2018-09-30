#pragma once
#include <vector>
#include "sequence_iterator_interface.hpp"

class ArpeggioEffectIterator : public SequenceIteratorInterface
{
public:
	ArpeggioEffectIterator(int second, int third);
	int getPosition() const override;
	int getSequenceType() const override;
	int getCommandType() const override;
	int getCommandData() const override;
	int next(bool isReleaseBegin = false) override;
	int front() override;

private:
	int pos_;
	int second_, third_;
};

class VibratoEffectIterator : public SequenceIteratorInterface
{
public:
	VibratoEffectIterator(int period, int depth);
	int getPosition() const override;
	int getSequenceType() const override;
	int getCommandType() const override;
	int getCommandData() const override;
	int next(bool isReleaseBegin = false) override;
	int front() override;

private:
	int pos_;
	std::vector<int> seq_;
};
