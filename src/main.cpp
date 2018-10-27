/* -----------------------------------------------------------------------------
 *
 * Giada - Your Hardcore Loopmachine
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2018 Giovanni A. Zuliani | Monocasual
 *
 * This file is part of Giada - Your Hardcore Loopmachine.
 *
 * Giada - Your Hardcore Loopmachine is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * Giada - Your Hardcore Loopmachine is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Giada - Your Hardcore Loopmachine. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------- */


#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#if defined(__linux__) || defined(__APPLE__)
	#include <unistd.h>
#endif
#include <FL/Fl.H>
#include "core/init.h"
#include "core/const.h"
#include "core/patch.h"
#include "core/conf.h"
#include "core/midiMapConf.h"
#include "core/mixer.h"
#include "core/clock.h"
#include "core/mixerHandler.h"
#include "core/kernelAudio.h"
#include "core/kernelMidi.h"
#include "core/recorder.h"
#include "core/queue.h"
#include "core/renderer.h"
#include "utils/gui.h"
#include "utils/time.h"
#include "gui/dialogs/gd_mainWindow.h"
#include "core/pluginHost.h"


std::atomic<bool> G_quit;
gdMainWindow*     G_MainWin;


giada::m::Queue<float, 8192> G_Queue;
std::mutex                   G_renderMutex;
std::condition_variable      G_renderCond;
bool                         G_renderReady;


void video()
{
	using namespace giada;

	if (m::kernelAudio::getStatus())
		while (G_quit.load() == false) {
			gu_refreshUI();
			u::time::sleep(G_GUI_REFRESH_RATE);
		}
}


int main(int argc, char** argv)
{
	using namespace giada;

	G_quit.store(false);

	init_prepareParser();
	init_prepareMidiMap();
	init_prepareKernelAudio();
	init_prepareKernelMIDI();
	init_startGUI(argc, argv);
	init_startKernelAudio();
	
	std::thread rendererThread(m::renderer::renderAudio);
	std::thread videoThread(video);

  Fl::lock();

#ifdef WITH_VST
	juce::initialiseJuce_GUI();
#endif

	int ret = Fl::run();

#ifdef WITH_VST
	juce::shutdownJuce_GUI();
#endif

	G_renderCond.notify_one(); // Unlock the render last time

	rendererThread.join();
	videoThread.join();
	
	return ret;
}



