//       _________ __                 __
//      /   _____//  |_____________ _/  |______     ____  __ __  ______
//      \_____  \\   __\_  __ \__  \\   __\__  \   / ___\|  |  \/  ___/
//      /        \|  |  |  | \// __ \|  |  / __ \_/ /_/  >  |  /\___ |
//     /_______  /|__|  |__|  (____  /__| (____  /\___  /|____//____  >
//             \/                  \/          \//_____/            \/
//  ______________________                           ______________________
//                        T H E   W A R   B E G I N S
//         Stratagus - A free fantasy real time strategy game engine
//
/**@name action_repair.cpp - The repair action. */
//
//      (c) Copyright 1999-2007 by Vladi Shabanski and Jimmy Salmon
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; only version 2 of the License.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
//      02111-1307, USA.
//

//@{

/*----------------------------------------------------------------------------
--  Includes
----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>

#include "stratagus.h"
#include "unittype.h"
#include "animation.h"
#include "player.h"
#include "unit.h"
#include "missile.h"
#include "actions.h"
#include "sound.h"
#include "tileset.h"
#include "map.h"
#include "pathfinder.h"
#include "interface.h"
#include "iolib.h"
#include "script.h"

/*----------------------------------------------------------------------------
--  Functions
----------------------------------------------------------------------------*/

/* virtual */ void COrder_Repair::Save(CFile &file, const CUnit &unit) const
{
	file.printf("{\"action-repair\",");

	if (this->Finished) {
		file.printf(" \"finished\", ");
	}
	file.printf(" \"range\", %d,", this->Range);
	if (this->HasGoal()) {
		CUnit &goal = *this->GetGoal();
		if (goal.Destroyed) {
			/* this unit is destroyed so it's not in the global unit
			 * array - this means it won't be saved!!! */
			printf ("FIXME: storing destroyed Goal - loading will fail.\n");
		}
		file.printf(" \"goal\", \"%s\",", UnitReference(goal).c_str());
	}
	file.printf(" \"tile\", {%d, %d},", this->goalPos.x, this->goalPos.y);

	file.printf(" \"repaircycle\", %d,", this->RepairCycle);
	file.printf(" \"state\", %d,\n  ", this->State);

	SaveDataMove(file);

	file.printf("}");
}

/* virtual */ bool COrder_Repair::ParseSpecificData(lua_State *l, int &j, const char *value, const CUnit &unit)
{
	if (this->ParseMoveData(l, j, value)) {
		return true;
	} else if (!strcmp("repaircycle", value)) {
		++j;
		lua_rawgeti(l, -1, j + 1);
		this->RepairCycle = LuaToNumber(l, -1);
		lua_pop(l, 1);
	} else if (!strcmp("state", value)) {
		++j;
		lua_rawgeti(l, -1, j + 1);
		this->State = LuaToNumber(l, -1);
		lua_pop(l, 1);
	} else {
		return false;
	}
	return true;
}



/**
**  Repair a unit.
**
**  @param unit  unit repairing
**  @param goal  unit being repaired
**
**  @return true when action is finished/canceled.
*/
bool COrder_Repair::RepairUnit(const CUnit &unit, CUnit &goal)
{
	if (goal.CurrentAction() == UnitActionBuilt) {
		COrder_Built &order = *static_cast<COrder_Built *>(goal.CurrentOrder());

		order.ProgressHp(goal, 100 * this->RepairCycle);
		this->RepairCycle = 0;
		return false;
	}
	if (goal.Variable[HP_INDEX].Value >= goal.Variable[HP_INDEX].Max) {
		return true;
	}
	CPlayer &player = *unit.Player;

	// Calculate the repair costs.
	Assert(goal.Stats->Variables[HP_INDEX].Max);

	// Check if enough resources are available
	for (int i = 1; i < MaxCosts; ++i) {
		if (player.Resources[i] < goal.Type->RepairCosts[i]) {
			player.Notify(NotifyYellow, unit.tilePos,
				_("We need more %s for repair!"), DefaultResourceNames[i].c_str());
			return true;
		}
	}
	// Subtract the resources
	player.SubCosts(goal.Type->RepairCosts);

	goal.Variable[HP_INDEX].Value += goal.Type->RepairHP;
	if (goal.Variable[HP_INDEX].Value >= goal.Variable[HP_INDEX].Max) {
		goal.Variable[HP_INDEX].Value = goal.Variable[HP_INDEX].Max;
		return true;
	}
	return false;
}

/**
**  Animate unit repair
**
**  @param unit  Unit, for that the repair animation is played.
*/
static void AnimateActionRepair(CUnit &unit)
{
	UnitShowAnimation(unit, unit.Type->Animations->Repair);
}

/* virtual */ void COrder_Repair::Execute(CUnit &unit)
{
	switch (this->State) {
		case 0:
			this->NewResetPath();
			this->State = 1;
			// FALL THROUGH
		case 1: { // Move near to target.
			// FIXME: RESET FIRST!! Why? We move first and than check if
			// something is in sight.
			int err = DoActionMove(unit);
			if (!unit.Anim.Unbreakable) {
				// No goal: if meeting damaged building repair it.
				CUnit *goal = this->GetGoal();

				if (goal) {
					if (!goal->IsVisibleAsGoal(*unit.Player)) {
						DebugPrint("repair target gone.\n");
						this->goalPos = goal->tilePos;
						this->ClearGoal();
						goal = NULL;
						this->NewResetPath();
					}
				} else if (unit.Player->AiEnabled) {
					// Ai players workers should stop if target is killed
					err = -1;
				}

				// Have reached target? FIXME: could use return value
				if (goal && unit.MapDistanceTo(*goal) <= unit.Type->RepairRange
					&& goal->Variable[HP_INDEX].Value < goal->Variable[HP_INDEX].Max) {
					unit.State = 0;
					this->State = 2;
					this->RepairCycle = 0;
					const Vec2i dir = goal->tilePos + goal->Type->GetHalfTileSize() - unit.tilePos;
					UnitHeadingFromDeltaXY(unit, dir);
				} else if (err < 0) {
					this->Finished = true;
					return ;
				}
			}
			break;
		}
		case 2: {// Repair the target.
			AnimateActionRepair(unit);
			this->RepairCycle++;
			if (unit.Anim.Unbreakable) {
				return ;
			}
			CUnit *goal = this->GetGoal();

			if (goal) {
				if (!goal->IsVisibleAsGoal(*unit.Player)) {
					DebugPrint("repair goal is gone\n");
					this->goalPos = goal->tilePos;
					// FIXME: should I clear this here?
					this->ClearGoal();
					goal = NULL;
					this->NewResetPath();
				} else {
					const int dist = unit.MapDistanceTo(*goal);

					if (dist <= unit.Type->RepairRange) {
						if (RepairUnit(unit, *goal)) {
							this->Finished = true;
							return ;
						}
					} else if (dist > unit.Type->RepairRange) {
						// If goal has move, chase after it
						unit.State = 0;
						this->State = 0;
					}
				}
			}
			// Target is fine, choose new one.
			if (!goal || goal->Variable[HP_INDEX].Value >= goal->Variable[HP_INDEX].Max) {
				this->Finished = true;
				return ;
			}
			// FIXME: automatic repair
		}
		break;
	}
}


//@}