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


#include <map>
#include <memory>
#include <vector>
#include <algorithm>
#include <cassert>
#include "../../utils/log.h"
#include "../action.h"
#include "../channel.h"
#include "recorder.h"


using std::map;
using std::vector;


namespace giada {
namespace m {
namespace recorder
{
using ActionMap = map<Frame, vector<Action*>>;

namespace
{
/* actions
The big map of actions {frame : actions[]}. This belongs to Recorder, but it
is often parsed by Mixer. So every "write" action performed on it (add, 
remove, ...) must be guarded by a trylock on the mixerMutex. Until a proper
lock-free solution will be implemented. */

ActionMap actions;

/* mixerMutex */

pthread_mutex_t* mixerMutex = nullptr;
bool active = false;


/* -------------------------------------------------------------------------- */


void trylock_(std::function<void()> f)
{
	assert(mixerMutex != nullptr);
	while (pthread_mutex_trylock(mixerMutex) == 0) {
		f();
		pthread_mutex_unlock(mixerMutex);
		break;
	}
}


/* -------------------------------------------------------------------------- */

/* optimize
Removes frames without actions. */

void optimize_(ActionMap& map)
{
	for (auto it = map.cbegin(); it != map.cend();)
		it->second.size() == 0 ? it = map.erase(it) : ++it;
}


/* -------------------------------------------------------------------------- */


void removeIf_(std::function<bool(const Action*)> f)
{
	ActionMap temp = actions;

	/*
	for (auto& kv : temp) {
		vector<Action*>& as = kv.second;
		as.erase(std::remove_if(as.begin(), as.end(), f), as.end());
	}*/
	for (auto& kv : temp) {
		auto i = std::begin(kv.second);
		while (i != std::end(kv.second)) {
			if (f(*i)) {
				delete *i;
				i = kv.second.erase(i);
			}
			else
				++i;
		}
	}
	optimize_(temp);

	trylock_([&](){ actions = temp; }); // TODO - or std::move(temp)?
}


/* -------------------------------------------------------------------------- */


/* updateKeyFrames_
Generates a copy (actually, a move) of the original map of actions by updating
the key frames in it, according to an external lambda function f. */

void updateKeyFrames_(std::function<Frame(Frame old)> f)
{
	/* TODO - This algorithm might be slow as f**k. Instead of updating keys in
	the existing map, we create a temporary map with the updated keys. Then we 
	swap it with the original one (moved, not copied). */
	
	ActionMap temp;

	for (auto& kv : actions)
	{
		Frame frame = f(kv.first);

		/* The value is the original array of actions stored in the old map. An
		update to all actions is required. Don't copy the vector, move it: we want
		to keep the original references. */

		temp[frame] = std::move(kv.second);
		for (Action* action : temp[frame])
			action->frame = frame;
	}

	trylock_([&](){ actions = std::move(temp); });
}


/* -------------------------------------------------------------------------- */


void debug_()
{
	puts("-------------");
	for (auto& kv : actions) {
		printf("frame: %d\n", kv.first);
		for (const Action* action : kv.second)
			printf(" this=%p - frame=%d, channel=%d, value=0x%X, next=%p\n", 
				(void*) action, action->frame, action->channel, action->event.getRaw(), (void*) action->next);	
	}
	puts("-------------");
}
} // {anonymous}


/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */


void init(pthread_mutex_t* m)
{
	mixerMutex = m;
	active = false;
	clearAll();
}


/* -------------------------------------------------------------------------- */


void clearAll()
{
	removeIf_([=](const Action* a) { return true; }); // TODO optimize this
}


/* -------------------------------------------------------------------------- */


void clearChannel(int channel)
{
	removeIf_([=](const Action* a) { return a->channel == channel; });
}


/* -------------------------------------------------------------------------- */


void clearActions(int channel, ActionType t)
{
	removeIf_([=](const Action* a)
	{ 
		return a->channel == channel && a->event.getStatus() == static_cast<int>(t);
	});
}


/* -------------------------------------------------------------------------- */


void deleteAction(const Action* target)
{
	removeIf_([=](const Action* a) { return a == target; });
}


/* -------------------------------------------------------------------------- */


bool hasActions(int channel)
{
	for (auto& kv : actions)
		for (const Action* action : kv.second)
			if (action->channel == channel)
				return true;
	return false;
}


/* -------------------------------------------------------------------------- */


bool isActive() { return active; }
void enable()   { active = true; }
void disable()  { active = false; }


/* -------------------------------------------------------------------------- */


const Action* rec(int channel, Frame frame, MidiEvent event, const Action* prev)
{
	ActionMap temp = actions;

	/* If key frame doesn't exist yet, the [] operator in std::map is smart enough 
	to insert a new item first. Then swap the temp map with the original one. */
	
	temp[frame].push_back(new Action{ channel, frame, event, nullptr, nullptr });
	
	/* Update the curr-next pointers in action, if a previous action has been
	provided. Cast away the constness of prev: yes, recorder is allowed to do it.
	Recorder is the sole owner and manager of all actions ;) */

	Action* curr = temp[frame].back();
	if (prev != nullptr) {
		const_cast<Action*>(prev)->next = curr;
		curr->prev = prev;
	}

//debug_();

	trylock_([&](){ actions = std::move(temp); });

//debug_();

	return curr;
}


/* -------------------------------------------------------------------------- */


void updateBpm(float oldval, float newval, int oldquanto)
{
	updateKeyFrames_([=](Frame old) 
	{
		/* The division here cannot be precise. A new frame can be 44099 and the 
		quantizer set to 44100. That would mean two recs completely useless. So we 
		compute a reject value ('scarto'): if it's lower than 6 frames the new frame 
		is collapsed with a quantized frame. FIXME - maybe 6 frames are too low. */
		Frame frame = (old / newval) * oldval;
		if (frame != 0) {
			Frame delta = oldquanto % frame;
			if (delta > 0 && delta <= 6)
				frame = frame + delta;
		}
		return frame;
	});
}


/* -------------------------------------------------------------------------- */


void updateSamplerate(int systemRate, int patchRate)
{
	if (systemRate == patchRate)
		return;

	float ratio = systemRate / (float) patchRate;

	updateKeyFrames_([=](Frame old) { return floorf(old * ratio); });
}


/* -------------------------------------------------------------------------- */


void expand(int old_fpb, int new_fpb)
{
	/* This algorithm requires multiple passages if we expand from e.g. 2 to 16 
	beats, precisely 16 / 2 - 1 = 7 times (-1 is the first group, which exists 
	yet). If we expand by a non-multiple, the result is zero, due to float->int 
	implicit cast. */
#if 0
	int pass = (int) (new_fpb / old_fpb) - 1;
	if (pass == 0) pass = 1;

	size_type init_fs = actions.size();

	for (unsigned z=1; z<=pass; z++) {
		for (unsigned i=0; i<init_fs; i++) {
			unsigned newframe = frames.at(i) + (old_fpb*z);
			frames.push_back(newframe);
			global.push_back(actions);
			for (unsigned k=0; k<global.at(i).size(); k++) {
				action* a = global.at(i).at(k);
				rec(a->chan, a->type, newframe, a->iValue, a->fValue);
			}
		}
	}
#endif
}


/* -------------------------------------------------------------------------- */


void shrink(int new_fpb)
{

}


/* -------------------------------------------------------------------------- */


vector<Action*> getActionsOnFrame(Frame frame)
{
	//assert(false); // TODO - must be implemented
	//vector<const Action*> out;
	return actions[frame];
	//return out;
}


/* -------------------------------------------------------------------------- */


vector<Action*> getActionsOnChannel(int channel)
{
	vector<Action*> out;
	forEachAction([&](Action* a)
	{
		if (a->channel == channel)
			out.push_back(a);
	});
	return out;
}


/* -------------------------------------------------------------------------- */


void forEachAction(std::function<void(Action*)> f)
{
	for (auto& kv : actions)
		for (Action* action : kv.second)
			f(action);
}
}}}; // giada::m::recorder::
