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

#include "opni_io.hpp"
#include "format/wopn_file.h"
#include "file_io_error.hpp"

namespace io
{
	OpniIO::OpniIO() : AbstractInstrumentIO("opni", "WOPN instrument", true, false) {}

	AbstractInstrument* OpniIO::load(const BinaryContainer& ctr, const std::string& fileName,
									std::weak_ptr<InstrumentsManager> instMan, int instNum) const
	{
		(void)fileName;
		OPNIFile opni;
		if (WOPN_LoadInstFromMem(&opni, const_cast<char*>(ctr.getPointer()), static_cast<size_t>(ctr.size())) != 0)
			throw FileCorruptionError(FileType::Inst, 0);

		return loadWOPNInstrument(opni.inst, instMan, instNum);
	}

	AbstractInstrument* OpniIO::loadWOPNInstrument(const WOPNInstrument &srcInst,
												   std::weak_ptr<InstrumentsManager> instMan,
												   int instNum)
	{
		std::shared_ptr<InstrumentsManager> instManLocked = instMan.lock();
		int envIdx = instManLocked->findFirstAssignableEnvelopeFM();
		if (envIdx < 0) throw FileCorruptionError(FileType::Bank, 0);
		const char *name = srcInst.inst_name;

		InstrumentFM* inst = new InstrumentFM(instNum, name, instManLocked.get());
		inst->setEnvelopeNumber(envIdx);
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::AL, srcInst.fbalg & 7);
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::FB, (srcInst.fbalg >> 3) & 7);

		const WOPNOperator *op[4] = {
			&srcInst.operators[0],
			&srcInst.operators[2],
			&srcInst.operators[1],
			&srcInst.operators[3],
		};

	#define LOAD_OPERATOR(n)						\
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::ML##n, op[n - 1]->dtfm_30 & 15); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::DT##n, (op[n - 1]->dtfm_30 >> 4) & 7); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::TL##n, op[n - 1]->level_40); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::KS##n, op[n - 1]->rsatk_50 >> 6); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::AR##n, op[n - 1]->rsatk_50 & 31); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::DR##n, op[n - 1]->amdecay1_60 & 31); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::SR##n, op[n - 1]->decay2_70 & 31); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::RR##n, op[n - 1]->susrel_80 & 15); \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::SL##n, op[n - 1]->susrel_80 >> 4); \
		int ssgeg##n = op[n - 1]->ssgeg_90; \
		ssgeg##n = ssgeg##n & 8 ? ssgeg##n & 7 : -1; \
		instManLocked->setEnvelopeFMParameter(envIdx, FMEnvelopeParameter::SSGEG##n, ssgeg##n); \
		int am##n = op[n - 1]->amdecay1_60 >> 7;

		LOAD_OPERATOR(1)
		LOAD_OPERATOR(2)
		LOAD_OPERATOR(3)
		LOAD_OPERATOR(4)

	#undef LOAD_OPERATOR

		if (srcInst.lfosens != 0) {
			int lfoIdx = instManLocked->findFirstAssignableLFOFM();
			if (lfoIdx < 0) throw FileCorruptionError(FileType::Bank, 0);
			inst->setLFOEnabled(true);
			inst->setLFONumber(lfoIdx);
			instManLocked->setLFOFMParameter(lfoIdx, FMLFOParameter::PMS, srcInst.lfosens & 7);
			instManLocked->setLFOFMParameter(lfoIdx, FMLFOParameter::AMS, (srcInst.lfosens >> 4) & 3);
			instManLocked->setLFOFMParameter(lfoIdx, FMLFOParameter::AM1, am1);
			instManLocked->setLFOFMParameter(lfoIdx, FMLFOParameter::AM2, am2);
			instManLocked->setLFOFMParameter(lfoIdx, FMLFOParameter::AM3, am3);
			instManLocked->setLFOFMParameter(lfoIdx, FMLFOParameter::AM4, am4);
		}

		if (srcInst.note_offset != 0) {
			int arpIdx = instManLocked->findFirstAssignableArpeggioFM();
			if (arpIdx < 0) throw FileCorruptionError(FileType::Bank, 0);
			inst->setArpeggioEnabled(FMOperatorType::All, true);
			inst->setArpeggioNumber(FMOperatorType::All, arpIdx);
			instManLocked->setArpeggioFMSequenceCommand(arpIdx, 0, srcInst.note_offset + 48, -1);
			instManLocked->setArpeggioFMType(arpIdx, SequenceType::ABSOLUTE_SEQUENCE);
		}

		return inst;
	}
}