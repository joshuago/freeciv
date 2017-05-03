/***********************************************************************
 Freeciv - Copyright (C) 1996-2013 - Freeciv Development Team
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include <math.h> /* ceil, floor */

/* utility */
#include "astring.h"

/* common */
#include "actions.h"
#include "city.h"
#include "combat.h"
#include "fc_interface.h"
#include "game.h"
#include "map.h"
#include "movement.h"
#include "unit.h"
#include "research.h"
#include "tile.h"

/* Custom data type for obligatory hard action requirements. */
struct obligatory_req {
  /* A requirement that contradicts the obligatory hard requirement. */
  struct requirement contradiction;

  /* Is the obligatory hard requirement in the action enabler's target
   * requirement vector? If FALSE it is in its actor requirement vector. */
  bool is_target;

  /* The error message to show when the hard obligatory requirement is
   * missing. Must be there. */
  const char *error_msg;
};

#define SPECVEC_TAG obligatory_req
#define SPECVEC_TYPE struct obligatory_req
#include "specvec.h"
#define obligatory_req_vector_iterate(obreq_vec, pobreq) \
  TYPED_VECTOR_ITERATE(struct obligatory_req, obreq_vec, pobreq)
#define obligatory_req_vector_iterate_end VECTOR_ITERATE_END

/* Values used to interpret action probabilities.
 *
 * Action probabilities are sent over the network. A change in a value here
 * will therefore change the network protocol.
 *
 * A change in a value here should also update the action probability
 * format documentation in fc_types.h */
/* The lowest possible probability value (0%) */
#define ACTPROB_VAL_MIN 0
/* The highest possible probability value (100%) */
#define ACTPROB_VAL_MAX 200
/* A probability increase of 1% corresponds to this increase. */
#define ACTPROB_VAL_1_PCT (ACTPROB_VAL_MAX / 100)
/* Action probability doesn't apply when min is this. */
#define ACTPROB_VAL_NA 253
/* Action probability unsupported when min is this. */
#define ACTPROB_VAL_NOT_IMPL 254

static struct action *actions[MAX_NUM_ACTIONS];
struct action_auto_perf auto_perfs[MAX_NUM_ACTION_AUTO_PERFORMERS];
static bool actions_initialized = FALSE;

static struct action_enabler_list *action_enablers_by_action[MAX_NUM_ACTIONS];

/* Hard requirements relates to action result. */
static struct obligatory_req_vector obligatory_hard_reqs[ACTION_COUNT];

static struct action *action_new(enum gen_action id,
                                 enum action_target_kind target_kind,
                                 bool hostile, bool requires_details,
                                 bool rare_pop_up,
                                 bool unitwaittime_controlled,
                                 const int min_distance,
                                 const int max_distance);

static bool is_enabler_active(const struct action_enabler *enabler,
			      const struct player *actor_player,
			      const struct city *actor_city,
			      const struct impr_type *actor_building,
			      const struct tile *actor_tile,
                              const struct unit *actor_unit,
			      const struct unit_type *actor_unittype,
			      const struct output_type *actor_output,
			      const struct specialist *actor_specialist,
			      const struct player *target_player,
			      const struct city *target_city,
			      const struct impr_type *target_building,
			      const struct tile *target_tile,
                              const struct unit *target_unit,
			      const struct unit_type *target_unittype,
			      const struct output_type *target_output,
			      const struct specialist *target_specialist);

static inline bool
action_prob_is_signal(const struct act_prob probability);
static inline bool
action_prob_not_relevant(const struct act_prob probability);
static inline bool
action_prob_not_impl(const struct act_prob probability);

/* Make sure that an action distance can target the whole map. */
FC_STATIC_ASSERT(MAP_DISTANCE_MAX <= ACTION_DISTANCE_LAST_NON_SIGNAL,
                 action_range_can_not_cover_the_whole_map);

/**************************************************************************
  Register an obligatory hard requirement for the actions it applies to.

  The vararg parameter is a list of action ids it applies to terminated
  by ACTION_NONE.
**************************************************************************/
static void oblig_hard_req_register(struct requirement contradiction,
                                    bool is_target,
                                    const char *error_message,
                                    ...)
{
  struct obligatory_req oreq;
  va_list args;
  enum gen_action act;

  /* A non null action message is used to indicate that an obligatory hard
   * requirement is missing. */
  fc_assert_ret(error_message);

  /* Pack the obligatory hard requirement. */
  oreq.contradiction = contradiction;
  oreq.is_target = is_target;
  oreq.error_msg = error_message;

  /* Add the obligatory hard requirement to each action it applies to. */
  va_start(args, error_message);

  while (ACTION_NONE != (act = va_arg(args, enum gen_action))) {
    /* Any invalid action result should terminate the loop before this
     * assertion. */
    fc_assert_ret_msg(gen_action_is_valid(act), "Invalid action id %d", act);

    obligatory_req_vector_append(&obligatory_hard_reqs[act], oreq);
  }

  va_end(args);
}

/**************************************************************************
  Hard code the obligatory hard requirements that don't depend on the rest
  of the ruleset. They are sorted by requirement to make it easy to read,
  modify and explain them.
**************************************************************************/
static void hard_code_oblig_hard_reqs(void)
{
  /* Why this is a hard requirement: There is currently no point in
   * allowing the listed actions against domestic targets.
   * (Possible counter argument: crazy hack involving the Lua
   * callback action_started_callback() to use an action to do
   * something else. */
  /* TODO: Unhardcode as a part of false flag operation support. */
  oblig_hard_req_register(req_from_values(VUT_DIPLREL, REQ_RANGE_LOCAL,
                                          FALSE, FALSE, TRUE, DRO_FOREIGN),
                          FALSE,
                          "All action enablers for %s must require a "
                          "foreign target.",
                          ACTION_ESTABLISH_EMBASSY,
                          ACTION_ESTABLISH_EMBASSY_STAY,
                          ACTION_SPY_INVESTIGATE_CITY,
                          ACTION_INV_CITY_SPEND,
                          ACTION_SPY_STEAL_GOLD,
                          ACTION_STEAL_MAPS,
                          ACTION_SPY_STEAL_TECH,
                          ACTION_SPY_TARGETED_STEAL_TECH,
                          ACTION_SPY_INCITE_CITY,
                          ACTION_SPY_BRIBE_UNIT,
                          ACTION_CAPTURE_UNITS,
                          ACTION_CONQUER_CITY,
                          ACTION_NONE);

  /* Why this is a hard requirement: there is a hard requirement that
   * the actor player is at war with the owner of any city on the
   * target tile. It can't move to the ruleset as long as Bombard is
   * targeted at unit stacks only. Having the same requirement
   * against each unit in the stack as against any city at the tile
   * ensures compatibility with any future solution that allows the
   * requirement against any city on the target tile to move to the
   * ruleset. */
  oblig_hard_req_register(req_from_values(VUT_DIPLREL, REQ_RANGE_LOCAL,
                                          FALSE, FALSE, TRUE, DS_WAR),
                          FALSE,
                          "All action enablers for %s must require a "
                          "target the actor is at war with.",
                          ACTION_BOMBARD, ACTION_NONE);

  /* Why this is a hard requirement: Keep the old rules. Need to work
   * out corner cases. */
  oblig_hard_req_register(req_from_values(VUT_DIPLREL, REQ_RANGE_LOCAL,
                                          FALSE, TRUE, TRUE, DRO_FOREIGN),
                          FALSE,
                          "All action enablers for %s must require a "
                          "domestic target.",
                          ACTION_UPGRADE_UNIT, ACTION_NONE);

  /* Why this is a hard requirement: Preserve semantics of NoHome
   * flag. Need to replace other uses in game engine before this can
   * be demoted to a regular unit flag. */
  oblig_hard_req_register(req_from_values(VUT_UTFLAG, REQ_RANGE_LOCAL,
                                          FALSE, TRUE, TRUE, UTYF_NOHOME),
                          FALSE,
                          "All action enablers for %s must require that "
                          "the actor doesn't have the NoHome utype flag.",
                          ACTION_HOME_CITY, ACTION_NONE);

  /* Why this is a hard requirement: Preserve semantics of NonMil
   * flag. Need to replace other uses in game engine before this can
   * be demoted to a regular unit flag. */
  oblig_hard_req_register(req_from_values(VUT_UTFLAG, REQ_RANGE_LOCAL,
                                          FALSE, TRUE, TRUE, UTYF_CIVILIAN),
                          FALSE,
                          "All action enablers for %s must require that "
                          "the actor doesn't have the NonMil utype flag.",
                          ACTION_ATTACK, ACTION_CONQUER_CITY, ACTION_NONE);

  /* Why this is a hard requirement: Preserve semantics of
   * CanOccupyCity unit class flag. */
  oblig_hard_req_register(req_from_values(VUT_UCFLAG, REQ_RANGE_LOCAL,
                                          FALSE, FALSE, TRUE,
                                          UCF_CAN_OCCUPY_CITY),
                          FALSE,
                          "All action enablers for %s must require that "
                          "the actor has the CanOccupyCity uclass flag.",
                          ACTION_CONQUER_CITY, ACTION_NONE);

  /* Why this is a hard requirement: Consistency with ACTION_ATTACK.
   * Assumed by other locations in the Freeciv code. Examples:
   * unit_move_to_tile_test() and unit_conquer_city(). */
  oblig_hard_req_register(req_from_values(VUT_DIPLREL, REQ_RANGE_LOCAL,
                                          FALSE, FALSE, TRUE, DS_WAR),
                          FALSE,
                          "All action enablers for %s must require that "
                          "the actor is at war with the target.",
                          ACTION_CONQUER_CITY, ACTION_NONE);

  /* Why this is a hard requirement: a unit must move into a city to
   * conquer it. */
  oblig_hard_req_register(req_from_values(VUT_MINMOVES, REQ_RANGE_LOCAL,
                                          FALSE, FALSE, TRUE, 1),
                          FALSE,
                          "All action enablers for %s must require that "
                          "the actor has a movement point left.",
                          ACTION_CONQUER_CITY, ACTION_NONE);

  /* Why this is a hard requirement: this eliminates the need to
   * check if units transported by the actor unit can exist at the
   * same tile as all the units in the occupied city.
   *
   * This makes an implicit rule explicit:
   * 1. A unit must move into a city to conquer it.
   * 2. It can't move into the city if the tile contains a non allied
   *    unit (see unit_move_to_tile_test()).
   * 3. A city could, at the time this rule was made explicit, only
   *    contain units allied to its owner.
   * 3. A player could, at the time this rule was made explicit, not
   *    be allied to a player that is at war with another ally.
   * 4. A player could, at the time this rule was made explicit, only
   *    conquer a city belonging to someone he was at war with.
   * Conclusion: the conquered city had to be empty.
   */
  oblig_hard_req_register(req_from_values(VUT_MAXTILEUNITS, REQ_RANGE_LOCAL,
                                          FALSE, FALSE, TRUE, 0),
                          TRUE,
                          "All action enablers for %s must require that "
                          "the target city is empty.",
                          ACTION_CONQUER_CITY, ACTION_NONE);

  /* Why this is a hard requirement: Assumed in the code. Corner case
   * where diplomacy prevents a transported unit to go to the target
   * tile. The paradrop code doesn't check if transported units can
   * coexist with the target tile city and units. */
  oblig_hard_req_register(req_from_values(VUT_UNITSTATE, REQ_RANGE_LOCAL,
                                          FALSE, TRUE, TRUE,
                                          USP_TRANSPORTING),
                          FALSE,
                          "All action enablers for %s must require that "
                          "the actor isn't transporting another unit.",
                          ACTION_PARADROP, ACTION_AIRLIFT, ACTION_NONE);
}

/**************************************************************************
  Hard code the obligatory hard requirements that needs access to the
  ruleset before they can be generated.
**************************************************************************/
static void hard_code_oblig_hard_reqs_ruleset(void)
{
  /* Why this is a hard requirement: the "animal can't conquer a city"
   * rule. Assumed in unit_can_take_over(). */
  nations_iterate(pnation) {
    if (nation_barbarian_type(pnation) == ANIMAL_BARBARIAN) {
      oblig_hard_req_register(req_from_values(VUT_NATION, REQ_RANGE_PLAYER,
                                              FALSE, TRUE, TRUE,
                                              nation_number(pnation)),
                              TRUE,
                              "All action enablers for %s must require a "
                              "non animal player actor.",
                              ACTION_CONQUER_CITY, ACTION_NONE);
    }
  } nations_iterate_end;
}

/**************************************************************************
  Hard code the actions.
**************************************************************************/
static void hard_code_actions(void)
{
  actions[ACTION_SPY_POISON] = action_new(ACTION_SPY_POISON, ATK_CITY,
                                          TRUE, FALSE, FALSE, TRUE,
                                          0, 1);
  actions[ACTION_SPY_SABOTAGE_UNIT] =
      action_new(ACTION_SPY_SABOTAGE_UNIT, ATK_UNIT,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_BRIBE_UNIT] =
      action_new(ACTION_SPY_BRIBE_UNIT, ATK_UNIT,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_SABOTAGE_CITY] =
      action_new(ACTION_SPY_SABOTAGE_CITY, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_TARGETED_SABOTAGE_CITY] =
      action_new(ACTION_SPY_TARGETED_SABOTAGE_CITY, ATK_CITY,
                 TRUE, TRUE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_INCITE_CITY] =
      action_new(ACTION_SPY_INCITE_CITY, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_ESTABLISH_EMBASSY] =
      action_new(ACTION_ESTABLISH_EMBASSY, ATK_CITY,
                 FALSE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_ESTABLISH_EMBASSY_STAY] =
      action_new(ACTION_ESTABLISH_EMBASSY_STAY, ATK_CITY,
                 FALSE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_STEAL_TECH] =
      action_new(ACTION_SPY_STEAL_TECH, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_TARGETED_STEAL_TECH] =
      action_new(ACTION_SPY_TARGETED_STEAL_TECH, ATK_CITY,
                 TRUE, TRUE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_INVESTIGATE_CITY] =
      action_new(ACTION_SPY_INVESTIGATE_CITY, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_INV_CITY_SPEND] =
      action_new(ACTION_INV_CITY_SPEND, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_SPY_STEAL_GOLD] =
      action_new(ACTION_SPY_STEAL_GOLD, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_TRADE_ROUTE] =
      action_new(ACTION_TRADE_ROUTE, ATK_CITY,
                 FALSE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_MARKETPLACE] =
      action_new(ACTION_MARKETPLACE, ATK_CITY,
                 FALSE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_HELP_WONDER] =
      action_new(ACTION_HELP_WONDER, ATK_CITY,
                 FALSE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_CAPTURE_UNITS] =
      action_new(ACTION_CAPTURE_UNITS, ATK_UNITS,
                 TRUE, FALSE, FALSE, TRUE,
                 /* A single domestic unit at the target tile will make the
                  * action illegal. It must therefore be performed from
                  * another tile. */
                 1, 1);
  actions[ACTION_FOUND_CITY] =
      action_new(ACTION_FOUND_CITY, ATK_TILE,
                 FALSE, FALSE, TRUE, TRUE,
                 /* Illegal to perform to a target on another tile.
                  * Reason: The Freeciv code assumes that the city founding
                  * unit is located at the tile were the new city is
                  * founded. */
                 0, 0);
  actions[ACTION_JOIN_CITY] =
      action_new(ACTION_JOIN_CITY, ATK_CITY,
                 FALSE, FALSE, TRUE, TRUE,
                 0, 1);
  actions[ACTION_STEAL_MAPS] =
      action_new(ACTION_STEAL_MAPS, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_BOMBARD] =
      action_new(ACTION_BOMBARD,
                 /* FIXME: Target is actually Units + City */
                 ATK_UNITS,
                 TRUE, FALSE, FALSE, TRUE,
                 /* A single domestic unit at the target tile will make the
                  * action illegal. It must therefore be performed from
                  * another tile. */
                 1,
                 /* Overwritten by the ruleset's bombard_max_range */
                 1);
  actions[ACTION_SPY_NUKE] =
      action_new(ACTION_SPY_NUKE, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_NUKE] =
      action_new(ACTION_NUKE,
                 /* FIXME: Target is actually Tile + Units + City */
                 ATK_TILE,
                 TRUE, FALSE, TRUE, TRUE,
                 0, 1);
  actions[ACTION_DESTROY_CITY] =
      action_new(ACTION_DESTROY_CITY, ATK_CITY,
                 TRUE, FALSE, TRUE, TRUE,
                 0, 1);
  actions[ACTION_EXPEL_UNIT] =
      action_new(ACTION_EXPEL_UNIT, ATK_UNIT,
                 TRUE, FALSE, FALSE, TRUE,
                 0, 1);
  actions[ACTION_RECYCLE_UNIT] =
      action_new(ACTION_RECYCLE_UNIT, ATK_CITY,
                 FALSE, FALSE, TRUE, TRUE,
                 /* Illegal to perform to a target on another tile to
                  * keep the rules exactly as they were for now. */
                 0, 1);
  actions[ACTION_DISBAND_UNIT] =
      action_new(ACTION_DISBAND_UNIT, ATK_SELF,
                 FALSE, FALSE, TRUE, TRUE,
                 0, 0);
  actions[ACTION_HOME_CITY] =
      action_new(ACTION_HOME_CITY, ATK_CITY,
                 FALSE, FALSE, TRUE, FALSE,
                 /* Illegal to perform to a target on another tile to
                  * keep the rules exactly as they were for now. */
                 0, 0);
  actions[ACTION_UPGRADE_UNIT] =
      action_new(ACTION_UPGRADE_UNIT, ATK_CITY,
                 FALSE, FALSE, TRUE, TRUE,
                 /* Illegal to perform to a target on another tile to
                  * keep the rules exactly as they were for now. */
                 0, 0);
  actions[ACTION_PARADROP] =
      action_new(ACTION_PARADROP, ATK_TILE,
                 FALSE, FALSE, TRUE, TRUE,
                 1,
                 /* Still limited by each unit type's paratroopers_range
                  * field. */
                 ACTION_DISTANCE_MAX);
  actions[ACTION_AIRLIFT] =
      action_new(ACTION_AIRLIFT, ATK_CITY,
                 FALSE, FALSE, TRUE, TRUE,
                 1, ACTION_DISTANCE_UNLIMITED);
  actions[ACTION_ATTACK] =
      action_new(ACTION_ATTACK,
                 /* FIXME: Target is actually City and, depending on the
                  * unreachable_protects setting, each unit at the target
                  * tile (Units) or any unit at the target tile. */
                 ATK_TILE,
                 TRUE, FALSE, FALSE, TRUE,
                 1, 1);
  actions[ACTION_CONQUER_CITY] =
      action_new(ACTION_CONQUER_CITY, ATK_CITY,
                 TRUE, FALSE, FALSE, TRUE,
                 1, 1);
  actions[ACTION_HEAL_UNIT] =
      action_new(ACTION_HEAL_UNIT, ATK_UNIT,
                 FALSE, FALSE, FALSE, TRUE,
                 0, 1);
}

/**************************************************************************
  Initialize the actions and the action enablers.
**************************************************************************/
void actions_init(void)
{
  int i, j;

  /* Hard code the actions */
  hard_code_actions();

  /* Initialize the action enabler list */
  action_iterate(act) {
    action_enablers_by_action[act] = action_enabler_list_new();
  } action_iterate_end;

  /* Initialize action obligatory hard requirements. */

  /* Obligatory hard requirements are sorted by action in memory. This makes
   * it easy to access the data. */
  action_iterate(act) {
    /* Prepare each action's storage area. */
    obligatory_req_vector_init(&obligatory_hard_reqs[act]);
  } action_iterate_end;

  /* Obligatory hard requirements are sorted by requirement in the source
   * code. This makes it easy to read, modify and explain it. */
  hard_code_oblig_hard_reqs();

  /* Initialize the action auto performers. */
  for (i = 0; i < MAX_NUM_ACTION_AUTO_PERFORMERS; i++) {
    /* Nothing here. Nothing after this point. */
    auto_perfs[i].cause = AAPC_COUNT;

    /* The criteria to pick *this* auto performer for its cause. */
    requirement_vector_init(&auto_perfs[i].reqs);

    for (j = 0; j < MAX_NUM_ACTIONS; j++) {
      /* Nothing here. Nothing after this point. */
      auto_perfs[i].alternatives[j] = ACTION_NONE;
    }
  }

  /* The actions them self are now initialized. */
  actions_initialized = TRUE;
}

/**************************************************************************
  Generate action related data based on the currently loaded ruleset. Done
  before ruleset sanity checking and ruleset compatibility post
  processing.
**************************************************************************/
void actions_rs_pre_san_gen(void)
{
  /* Some obligatory hard requirements needs access to the rest of the
   * ruleset. */
  hard_code_oblig_hard_reqs_ruleset();
}

/**************************************************************************
  Free the actions and the action enablers.
**************************************************************************/
void actions_free(void)
{
  int i;

  /* Don't consider the actions to be initialized any longer. */
  actions_initialized = FALSE;

  action_iterate(act) {
    action_enabler_list_iterate(action_enablers_by_action[act], enabler) {
      requirement_vector_free(&enabler->actor_reqs);
      requirement_vector_free(&enabler->target_reqs);
      free(enabler);
    } action_enabler_list_iterate_end;

    action_enabler_list_destroy(action_enablers_by_action[act]);

    FC_FREE(actions[act]);
  } action_iterate_end;

  /* Free the obligatory hard action requirements. */
  action_iterate(act) {
    obligatory_req_vector_free(&obligatory_hard_reqs[act]);
  } action_iterate_end;

  /* Free the action auto performers. */
  for (i = 0; i < MAX_NUM_ACTION_AUTO_PERFORMERS; i++) {
    requirement_vector_free(&auto_perfs[i].reqs);
  }
}

/**************************************************************************
  Returns TRUE iff the actions are initialized.

  Doesn't care about action enablers.
**************************************************************************/
bool actions_are_ready(void)
{
  if (!actions_initialized) {
    /* The actions them self aren't initialized yet. */
    return FALSE;
  }

  action_iterate(act) {
    if (actions[act]->ui_name[0] == '\0') {
      /* An action without a UI name exists means that the ruleset haven't
       * loaded yet. The ruleset loading will assign a default name to
       * any actions not named by the ruleset. The client will get this
       * name from the server. */
      return FALSE;
    }
  } action_iterate_end;

  /* The actions should be ready for use. */
  return TRUE;
}

/**************************************************************************
  Create a new action.
**************************************************************************/
static struct action *action_new(enum gen_action id,
                                 enum action_target_kind target_kind,
                                 bool hostile, bool requires_details,
                                 bool rare_pop_up,
                                 bool unitwaittime_controlled,
                                 const int min_distance,
                                 const int max_distance)
{
  struct action *action;

  action = fc_malloc(sizeof(*action));

  action->id = id;
  action->actor_kind = AAK_UNIT;
  action->target_kind = target_kind;

  action->hostile = hostile;
  action->requires_details = requires_details;
  action->rare_pop_up = rare_pop_up;

  /* The distance between the actor and itself is always 0. */
  fc_assert(target_kind != ATK_SELF
            || (min_distance == 0 && max_distance == 0));

  action->min_distance = min_distance;
  action->max_distance = max_distance;

  action->unitwaittime_controlled = unitwaittime_controlled;

  /* Loaded from the ruleset. Until generalized actions are ready it has to
   * be defined seperatly from other action data. */
  action->ui_name[0] = '\0';
  action->quiet = FALSE;
  BV_CLR_ALL(action->blocked_by);

  return action;
}

/**************************************************************************
  Returns TRUE iff the specified action ID refers to a valid action.
**************************************************************************/
bool action_id_exists(const int action_id)
{
  /* Actions are still hard coded. */
  return gen_action_is_valid(action_id) && actions[action_id];
}

/**************************************************************************
  Return the action with the given id.

  Returns NULL if no action with the given id exists.
**************************************************************************/
struct action *action_by_number(int action_id)
{
  if (!action_id_exists(action_id)) {
    /* Nothing to return. */

    log_verbose("Asked for non existing action numbered %d", action_id);

    return NULL;
  }

  fc_assert_msg(actions[action_id], "Action %d don't exist.", action_id);

  return actions[action_id];
}

/**************************************************************************
  Return the action with the given name.

  Returns NULL if no action with the given name exists.
**************************************************************************/
struct action *action_by_rule_name(const char *name)
{
  /* Actions are still hard coded in the gen_action enum. */
  int action_id = gen_action_by_name(name, fc_strcasecmp);

  if (!action_id_exists(action_id)) {
    /* Nothing to return. */

    log_verbose("Asked for non existing action named %s", name);

    return NULL;
  }

  return action_by_number(action_id);
}

/**************************************************************************
  Get the actor kind of an action.
**************************************************************************/
enum action_actor_kind action_get_actor_kind(const struct action *paction)
{
  fc_assert_ret_val_msg(paction, AAK_COUNT, "Action doesn't exist.");

  return paction->actor_kind;
}

/**************************************************************************
  Get the target kind of an action.
**************************************************************************/
enum action_target_kind action_get_target_kind(
    const struct action *paction)
{
  fc_assert_ret_val_msg(paction, ATK_COUNT, "Action doesn't exist.");

  return paction->target_kind;
}

/**************************************************************************
  Returns TRUE iff performing the specified action has the specified
  result.
**************************************************************************/
bool action_has_result(struct action *paction, enum gen_action result)
{
  /* The action result is currently used as the action id. */
  return paction->id == result;
}

/**************************************************************************
  Returns TRUE iff the specified action is hostile.
**************************************************************************/
bool action_is_hostile(int action_id)
{
  fc_assert_msg(actions[action_id], "Action %d don't exist.", action_id);

  return actions[action_id]->hostile;
}

/**************************************************************************
  Returns TRUE iff the specified action REQUIRES the player to provide
  details in addition to actor and target. Returns FALSE if the action
  doesn't support any additional details or if they can be set by Freeciv
  it self.
**************************************************************************/
bool action_requires_details(int action_id)
{
  fc_assert_msg(actions[action_id], "Action %d don't exist.", action_id);

  return actions[action_id]->requires_details;
}

/**************************************************************************
  Returns TRUE iff a unit's ability to perform this action will pop up the
  action selection dialog before the player asks for it only in exceptional
  cases.

  An example of an exceptional case is when the player tries to move a
  unit to a tile it can't move to but can perform this action to.
**************************************************************************/
bool action_id_is_rare_pop_up(int action_id)
{
  fc_assert_ret_val_msg((action_id_exists(action_id)),
                        FALSE, "Action %d don't exist.", action_id);

  return actions[action_id]->rare_pop_up;
}

/**************************************************************************
  Returns TRUE iff the specified distance between actor and target is
  sm,aller or equal to the max range accepted by the specified action.
**************************************************************************/
bool action_distance_inside_max(const struct action *action,
                                const int distance)
{
  return (distance <= action->max_distance
          || action->max_distance == ACTION_DISTANCE_UNLIMITED);
}

/**************************************************************************
  Returns TRUE iff the specified distance between actor and target is
  within the range acceptable to the specified action.
**************************************************************************/
bool action_distance_accepted(const struct action *action,
                              const int distance)
{
  fc_assert_ret_val(action, FALSE);

  return (distance >= action->min_distance
          && action_distance_inside_max(action, distance));
}

/**************************************************************************
  Returns TRUE iff blocked will be illegal if blocker is legal.
**************************************************************************/
bool action_would_be_blocked_by(const struct action *blocked,
                                const struct action *blocker)
{
  fc_assert_ret_val(blocked, FALSE);
  fc_assert_ret_val(blocker, FALSE);

  return BV_ISSET(blocked->blocked_by, action_number(blocker));
}

/**************************************************************************
  Get the universal number of the action.
**************************************************************************/
int action_number(const struct action *action)
{
  return action->id;
}

/**************************************************************************
  Get the rule name of the action.
**************************************************************************/
const char *action_rule_name(const struct action *action)
{
  /* Rule name is still hard coded. */
  return action_id_rule_name(action->id);
}

/**************************************************************************
  Get the action name used when displaying the action in the UI. Nothing
  is added to the UI name.
**************************************************************************/
const char *action_name_translation(const struct action *action)
{
  /* Use action_id_name_translation() to format the UI name. */
  return action_id_name_translation(action->id);
}

/**************************************************************************
  Get the rule name of the action.
**************************************************************************/
const char *action_id_rule_name(int action_id)
{
  fc_assert_msg(actions[action_id], "Action %d don't exist.", action_id);

  return gen_action_name(action_id);
}

/**************************************************************************
  Get the action name used when displaying the action in the UI. Nothing
  is added to the UI name.
**************************************************************************/
const char *action_id_name_translation(int action_id)
{
  return action_prepare_ui_name(action_id, "", ACTPROB_NA, NULL);
}

/**************************************************************************
  Get the action name with a mnemonic ready to display in the UI.
**************************************************************************/
const char *action_get_ui_name_mnemonic(int action_id,
                                        const char* mnemonic)
{
  return action_prepare_ui_name(action_id, mnemonic, ACTPROB_NA, NULL);
}

/**************************************************************************
  Get the UI name ready to show the action in the UI. It is possible to
  add a client specific mnemonic. Success probability information is
  interpreted and added to the text. A custom text can be inserted before
  the probability information.
**************************************************************************/
const char *action_prepare_ui_name(int action_id, const char* mnemonic,
                                   const struct act_prob prob,
                                   const char* custom)
{
  static struct astring str = ASTRING_INIT;
  static struct astring chance = ASTRING_INIT;

  /* Text representation of the probability. */
  const char* probtxt;

  if (!actions_are_ready()) {
    /* Could be a client who haven't gotten the ruleset yet */

    /* so there shouldn't be any action probability to show */
    fc_assert(action_prob_not_relevant(prob));

    /* but the action should be valid */
    fc_assert_ret_val_msg(action_id_exists(action_id),
                          "Invalid action",
                          "Invalid action %d", action_id);

    /* and no custom text will be inserted */
    fc_assert(custom == NULL || custom[0] == '\0');

    /* Make the best of what is known */
    astr_set(&str, _("%s%s (name may be wrong)"),
             mnemonic, gen_action_name(action_id));

    /* Return the guess. */
    return astr_str(&str);
  }

  /* How to interpret action probabilities like prob is documented in
   * fc_types.h */
  if (action_prob_is_signal(prob)) {
    fc_assert(action_prob_not_impl(prob)
              || action_prob_not_relevant(prob));

    /* Unknown because of missing server support or should not exits. */
    probtxt = NULL;
  } else {
    if (prob.min == prob.max) {
      /* Only one probability in range. */

      /* TRANS: the probability that an action will succeed. Given in
       * percentage. Resolution is 0.5%. */
      astr_set(&chance, _("%.1f%%"), (double)prob.max / ACTPROB_VAL_1_PCT);
    } else {
      /* TRANS: the interval (end points included) where the probability of
       * the action's success is. Given in percentage. Resolution is 0.5%. */
      astr_set(&chance, _("[%.1f%%, %.1f%%]"),
               (double)prob.min / ACTPROB_VAL_1_PCT,
               (double)prob.max / ACTPROB_VAL_1_PCT);
    }
    probtxt = astr_str(&chance);
  }

  /* Format the info part of the action's UI name. */
  if (probtxt != NULL && custom != NULL) {
    /* TRANS: action UI name's info part with custom info and probability.
     * Hint: you can move the paren handling from this sting to the action
     * names if you need to add extra information (like a mnemonic letter
     * that doesn't appear in the action UI name) to it. In that case you
     * must do so for all strings with this comment and for every action
     * name. To avoid a `()` when no UI name info part is added you have
     * to add the extra information to every action name or remove the
     * surrounding parens. */
    astr_set(&chance, _(" (%s; %s)"), custom, probtxt);
  } else if (probtxt != NULL) {
    /* TRANS: action UI name's info part with probability.
     * Hint: you can move the paren handling from this sting to the action
     * names if you need to add extra information (like a mnemonic letter
     * that doesn't appear in the action UI name) to it. In that case you
     * must do so for all strings with this comment and for every action
     * name. To avoid a `()` when no UI name info part is added you have
     * to add the extra information to every action name or remove the
     * surrounding parens. */
    astr_set(&chance, _(" (%s)"), probtxt);
  } else if (custom != NULL) {
    /* TRANS: action UI name's info part with custom info.
     * Hint: you can move the paren handling from this sting to the action
     * names if you need to add extra information (like a mnemonic letter
     * that doesn't appear in the action UI name) to it. In that case you
     * must do so for all strings with this comment and for every action
     * name. To avoid a `()` when no UI name info part is added you have
     * to add the extra information to every action name or remove the
     * surrounding parens. */
    astr_set(&chance, _(" (%s)"), custom);
  } else {
    /* No info part to display. */
    astr_clear(&chance);
  }

  fc_assert_msg(actions[action_id], "Action %d don't exist.", action_id);

  astr_set(&str, _(actions[action_id]->ui_name), mnemonic,
           astr_str(&chance));

  return astr_str(&str);
}

/**************************************************************************
  Get information about starting the action in the current situation.
  Suitable for a tool tip for the button that starts it.
**************************************************************************/
const char *action_get_tool_tip(const int action_id,
                                const struct act_prob prob)
{
  static struct astring tool_tip = ASTRING_INIT;

  if (action_prob_is_signal(prob)) {
    fc_assert(action_prob_not_impl(prob));

    /* Missing server support. No in game action will change this. */
    astr_clear(&tool_tip);
  } else if (prob.min == prob.max) {
    /* TRANS: action probability of success. Given in percentage.
     * Resolution is 0.5%. */
    astr_set(&tool_tip, _("The probability of success is %.1f%%."),
             (double)prob.max / ACTPROB_VAL_1_PCT);
  } else {
    astr_set(&tool_tip,
             /* TRANS: action probability range (min to max). Given in
              * percentage. Resolution is 0.5%. */
             _("The probability of success is %.1f%%, %.1f%% or somewhere"
               " in between."),
             (double)prob.min / ACTPROB_VAL_1_PCT,
             (double)prob.max / ACTPROB_VAL_1_PCT);
  }

  return astr_str(&tool_tip);
}

/**************************************************************************
  Get the unit type role corresponding to the ability to do the specified
  action.
**************************************************************************/
int action_get_role(const struct action *paction)
{
  fc_assert_msg(AAK_UNIT == action_get_actor_kind(paction),
                "Action %s isn't performed by a unit",
                action_rule_name(paction));

  return paction->id + L_LAST;
}

/**************************************************************************
  Create a new action enabler.
**************************************************************************/
struct action_enabler *action_enabler_new(void)
{
  struct action_enabler *enabler;

  enabler = fc_malloc(sizeof(*enabler));
  enabler->disabled = FALSE;
  requirement_vector_init(&enabler->actor_reqs);
  requirement_vector_init(&enabler->target_reqs);

  /* Make sure that action doesn't end up as a random value that happens to
   * be a valid action id. */
  enabler->action = ACTION_NONE;

  return enabler;
}

/**************************************************************************
  Create a new copy of an existing action enabler.
**************************************************************************/
struct action_enabler *
action_enabler_copy(const struct action_enabler *original)
{
  struct action_enabler *enabler = action_enabler_new();

  enabler->action = original->action;

  requirement_vector_copy(&enabler->actor_reqs, &original->actor_reqs);
  requirement_vector_copy(&enabler->target_reqs, &original->target_reqs);

  return enabler;
}

/**************************************************************************
  Add an action enabler to the current ruleset.
**************************************************************************/
void action_enabler_add(struct action_enabler *enabler)
{
  /* Sanity check: a non existing action enabler can't be added. */
  fc_assert_ret(enabler);
  /* Sanity check: a non existing action doesn't have enablers. */
  fc_assert_ret(action_id_exists(enabler->action));

  action_enabler_list_append(
        action_enablers_for_action(enabler->action),
        enabler);
}

/**************************************************************************
  Remove an action enabler from the current ruleset.

  Returns TRUE on success.
**************************************************************************/
bool action_enabler_remove(struct action_enabler *enabler)
{
  /* Sanity check: a non existing action enabler can't be removed. */
  fc_assert_ret_val(enabler, FALSE);
  /* Sanity check: a non existing action doesn't have enablers. */
  fc_assert_ret_val(action_id_exists(enabler->action), FALSE);

  return action_enabler_list_remove(
        action_enablers_for_action(enabler->action),
        enabler);
}

/**************************************************************************
  Get all enablers for an action in the current ruleset.
**************************************************************************/
struct action_enabler_list *
action_enablers_for_action(enum gen_action action)
{
  /* Sanity check: a non existing action doesn't have enablers. */
  fc_assert_ret_val(action_id_exists(action), NULL);

  return action_enablers_by_action[action];
}

/**************************************************************************
  Returns an error message text if the action enabler is missing at least
  one of its action's obligatory hard requirement. Returns NULL if all
  obligatory hard requirements are there.

  An action may force its enablers to include one or more of its hard
  requirements. (See the section "Actions and their hard requirements" of
  doc/README.actions)

  This doesn't include those of the action's hard requirements that can't
  be expressed as a requirement vector or hard requirements that the
  action doesn't force enablers to include.
**************************************************************************/
const char *
action_enabler_obligatory_reqs_missing(struct action_enabler *enabler)
{
  /* Sanity check: a non existing action enabler is missing but it doesn't
   * miss any obligatory hard requirements. */
  fc_assert_ret_val(enabler, NULL);

  /* Sanity check: a non existing action doesn't have any obligatory hard
   * requirements. */
  fc_assert_ret_val(action_id_exists(enabler->action), NULL);

  obligatory_req_vector_iterate(&obligatory_hard_reqs[enabler->action],
                                obreq) {
    struct requirement_vector *ae_vec;

    /* Select action enabler requirement vector. */
    ae_vec = (obreq->is_target ? &enabler->target_reqs :
                                 &enabler->actor_reqs);

    if (!does_req_contradicts_reqs(&obreq->contradiction, ae_vec)) {
      /* Sanity check: doesn't return NULL when a problem is detected. */
      fc_assert_ret_val(obreq->error_msg,
                        "Missing obligatory hard requirement for %s.");

      return obreq->error_msg;
    }
  } obligatory_req_vector_iterate_end;

  /* No missing obligatory hard requirements. */
  return NULL;
}

/**************************************************************************
  Inserts any missing obligatory hard requirements in the action enabler
  based on its action.

  See action_enabler_obligatory_reqs_missing()
**************************************************************************/
void action_enabler_obligatory_reqs_add(struct action_enabler *enabler)
{
  /* Sanity check: a non existing action enabler is missing but it doesn't
   * miss any obligatory hard requirements. */
  fc_assert_ret(enabler);

  /* Sanity check: a non existing action doesn't have any obligatory hard
   * requirements. */
  fc_assert_ret(action_id_exists(enabler->action));

  obligatory_req_vector_iterate(&obligatory_hard_reqs[enabler->action],
                                obreq) {
    struct requirement_vector *ae_vec;

    /* Select action enabler requirement vector. */
    ae_vec = (obreq->is_target ? &enabler->target_reqs :
                                 &enabler->actor_reqs);

    if (!does_req_contradicts_reqs(&obreq->contradiction, ae_vec)) {
      struct requirement missing;

      /* Change the requirement from what should conflict to what is
       * wanted. */
      missing.present = !obreq->contradiction.present;
      missing.source = obreq->contradiction.source;
      missing.range = obreq->contradiction.range;
      missing.survives = obreq->contradiction.survives;
      missing.quiet = obreq->contradiction.quiet;

      /* Insert the missing requirement. */
      requirement_vector_append(ae_vec, missing);
    }
  } obligatory_req_vector_iterate_end;

  /* Remove anything that conflicts with the newly added reqs. */
  requirement_vector_contradiction_clean(&enabler->actor_reqs);
  requirement_vector_contradiction_clean(&enabler->target_reqs);

  /* Sanity check: obligatory requirement insertion should have fixed the
   * action enabler. */
  fc_assert(action_enabler_obligatory_reqs_missing(enabler) == NULL);
}

/**************************************************************************
  Returns TRUE iff the specified player knows (has seen) the specified
  tile.
**************************************************************************/
static bool plr_knows_tile(const struct player *plr,
                           const struct tile *ttile)
{
  return plr && ttile && (tile_get_known(ttile, plr) != TILE_UNKNOWN);
}

/**************************************************************************
  Returns TRUE iff the specified player can see the specified tile.
**************************************************************************/
static bool plr_sees_tile(const struct player *plr,
                          const struct tile *ttile)
{
  return plr && ttile && (tile_get_known(ttile, plr) == TILE_KNOWN_SEEN);
}

/**************************************************************************
  Returns the local building type of a city target.

  target_city can't be NULL
**************************************************************************/
static struct impr_type *
tgt_city_local_building(const struct city *target_city)
{
  /* Only used with city targets */
  fc_assert_ret_val(target_city, NULL);

  if (target_city->production.kind == VUT_IMPROVEMENT) {
    /* The local building is what the target city currently is building. */
    return target_city->production.value.building;
  } else {
    /* In the current semantic the local building is always the building
     * being built. */
    /* TODO: Consider making the local building the target building for
     * actions that allows specifying a building target. */
    return NULL;
  }
}

/**************************************************************************
  Returns the local unit type of a city target.

  target_city can't be NULL
**************************************************************************/
static struct unit_type *
tgt_city_local_utype(const struct city *target_city)
{
  /* Only used with city targets */
  fc_assert_ret_val(target_city, NULL);

  if (target_city->production.kind == VUT_UTYPE) {
    /* The local unit type is what the target city currently is
     * building. */
    return target_city->production.value.utype;
  } else {
    /* In the current semantic the local utype is always the type of the
     * unit being built. */
    return NULL;
  }
}

/**************************************************************************
  Returns the target tile for actions that may block the specified action.
  This is needed because some actions can be blocked by an action with a
  different target kind. The target tile could therefore be missing.

  Example: The ATK_SELF action ACTION_DISBAND_UNIT can be blocked by the
  ATK_CITY action ACTION_RECYCLE_UNIT.
**************************************************************************/
static const struct tile *
blocked_find_target_tile(const int action_id,
                         const struct unit *actor_unit,
                         const struct tile *target_tile_arg,
                         const struct city *target_city,
                         const struct unit *target_unit)
{
  if (target_tile_arg != NULL) {
    /* Trust the caller. */
    return target_tile_arg;
  }

  switch (action_id_get_target_kind(action_id)) {
  case ATK_CITY:
    fc_assert_ret_val(target_city, NULL);
    return city_tile(target_city);
  case ATK_UNIT:
    fc_assert_ret_val(target_unit, NULL);
    return unit_tile(target_unit);
  case ATK_UNITS:
    fc_assert_ret_val(target_unit || target_tile_arg, NULL);
    if (target_unit) {
      return unit_tile(target_unit);
    }
    /* Fall through. */
  case ATK_TILE:
    fc_assert_ret_val(target_tile_arg, NULL);
    return target_tile_arg;
  case ATK_SELF:
    fc_assert_ret_val(actor_unit, NULL);
    return unit_tile(actor_unit);
  case ATK_COUNT:
    /* Handled below. */
    break;
  }

  fc_assert_msg(FALSE, "Bad action target kind %d for action %d",
                action_id_get_target_kind(action_id), action_id);
  return NULL;
}

/**************************************************************************
  Returns the target city for actions that may block the specified action.
  This is needed because some actions can be blocked by an action with a
  different target kind. The target city argument could therefore be
  missing.

  Example: The ATK_SELF action ACTION_DISBAND_UNIT can be blocked by the
  ATK_CITY action ACTION_RECYCLE_UNIT.
**************************************************************************/
static const struct city *
blocked_find_target_city(const int action_id,
                         const struct unit *actor_unit,
                         const struct tile *target_tile,
                         const struct city *target_city_arg,
                         const struct unit *target_unit)
{
  if (target_city_arg != NULL) {
    /* Trust the caller. */
    return target_city_arg;
  }

  switch (action_id_get_target_kind(action_id)) {
  case ATK_CITY:
    fc_assert_ret_val(target_city_arg, NULL);
    return target_city_arg;
  case ATK_UNIT:
    fc_assert_ret_val(target_unit, NULL);
    fc_assert_ret_val(unit_tile(target_unit), NULL);
    return tile_city(unit_tile(target_unit));
  case ATK_UNITS:
    fc_assert_ret_val(target_unit || target_tile, NULL);
    if (target_unit) {
      fc_assert_ret_val(unit_tile(target_unit), NULL);
      return tile_city(unit_tile(target_unit));
    }
    /* Fall through. */
  case ATK_TILE:
    fc_assert_ret_val(target_tile, NULL);
    return tile_city(target_tile);
  case ATK_SELF:
    fc_assert_ret_val(actor_unit, NULL);
    fc_assert_ret_val(unit_tile(actor_unit), NULL);
    return tile_city(unit_tile(actor_unit));
  case ATK_COUNT:
    /* Handled below. */
    break;
  }

  fc_assert_msg(FALSE, "Bad action target kind %d for action %d",
                action_id_get_target_kind(action_id), action_id);
  return NULL;
}

/**************************************************************************
  Returns the action that blocks the specified action or NULL if the
  specified action isn't blocked.

  An action that can block another blocks when it is forced and possible.
**************************************************************************/
struct action *action_is_blocked_by(const int action_id,
                                    const struct unit *actor_unit,
                                    const struct tile *target_tile_arg,
                                    const struct city *target_city_arg,
                                    const struct unit *target_unit)
{


  const struct tile *target_tile
      = blocked_find_target_tile(action_id, actor_unit, target_tile_arg,
                                 target_city_arg, target_unit);
  const struct city *target_city
      = blocked_find_target_city(action_id, actor_unit, target_tile,
                                 target_city_arg, target_unit);

  action_iterate(blocker_id) {
    fc_assert_action(action_id_get_actor_kind(blocker_id) == AAK_UNIT,
                     continue);

    if (!action_id_would_be_blocked_by(action_id, blocker_id)) {
      /* It doesn't matter if it is legal. It won't block the action. */
      continue;
    }

    switch (action_id_get_target_kind(blocker_id)) {
    case ATK_CITY:
      if (!target_city) {
        /* Can't be enabled. No target. */
        continue;
      }
      if (is_action_enabled_unit_on_city(blocker_id,
                                         actor_unit, target_city)) {
        return action_by_number(blocker_id);
      }
      break;
    case ATK_UNIT:
      if (!target_unit) {
        /* Can't be enabled. No target. */
        continue;
      }
      if (is_action_enabled_unit_on_unit(blocker_id,
                                         actor_unit, target_unit)) {
        return action_by_number(blocker_id);
      }
      break;
    case ATK_UNITS:
      if (!target_tile) {
        /* Can't be enabled. No target. */
        continue;
      }
      if (is_action_enabled_unit_on_units(blocker_id,
                                          actor_unit, target_tile)) {
        return action_by_number(blocker_id);
      }
      break;
    case ATK_TILE:
      if (!target_tile) {
        /* Can't be enabled. No target. */
        continue;
      }
      if (is_action_enabled_unit_on_tile(blocker_id,
                                         actor_unit, target_tile)) {
        return action_by_number(blocker_id);
      }
      break;
    case ATK_SELF:
      if (is_action_enabled_unit_on_self(blocker_id, actor_unit)) {
        return action_by_number(blocker_id);
      }
      break;
    case ATK_COUNT:
      fc_assert_action(action_id_get_target_kind(blocker_id) != ATK_COUNT,
                       continue);
      break;
    }
  } action_iterate_end;

  /* Not blocked. */
  return NULL;
}

/**************************************************************************
  Returns TRUE if the specified unit type can perform the wanted action
  given that an action enabler later will enable it.

  This is done by checking the action's hard requirements. Hard
  requirements must be TRUE before an action can be done. The reason why
  is usually that code dealing with the action assumes that the
  requirements are true. A requirement may also end up here if it can't be
  expressed in a requirement vector or if its absence makes the action
  pointless.

  When adding a new hard requirement here:
   * explain why it is a hard requirement in a comment.
**************************************************************************/
bool
action_actor_utype_hard_reqs_ok(const enum gen_action wanted_action,
                                const struct unit_type *actor_unittype)
{
  switch (wanted_action) {
  case ACTION_JOIN_CITY:
    if (utype_pop_value(actor_unittype) <= 0) {
      /* Reason: Must have population to add. */
      return FALSE;
    }
    break;

  case ACTION_BOMBARD:
    if (actor_unittype->bombard_rate <= 0) {
      /* Reason: Can't bombard if it never fires. */
      return FALSE;
    }

    if (actor_unittype->attack_strength <= 0) {
      /* Reason: Can't bombard without attack strength. */
      return FALSE;
    }

    break;

  case ACTION_UPGRADE_UNIT:
    if (actor_unittype->obsoleted_by == U_NOT_OBSOLETED) {
      /* Reason: Nothing to upgrade to. */
      return FALSE;
    }
    break;

  case ACTION_ATTACK:
    if (actor_unittype->attack_strength <= 0) {
      /* Reason: Can't attack without strength. */
      return FALSE;
    }
    break;

  case ACTION_ESTABLISH_EMBASSY:
  case ACTION_ESTABLISH_EMBASSY_STAY:
  case ACTION_SPY_INVESTIGATE_CITY:
  case ACTION_INV_CITY_SPEND:
  case ACTION_SPY_POISON:
  case ACTION_SPY_STEAL_GOLD:
  case ACTION_SPY_SABOTAGE_CITY:
  case ACTION_SPY_TARGETED_SABOTAGE_CITY:
  case ACTION_SPY_STEAL_TECH:
  case ACTION_SPY_TARGETED_STEAL_TECH:
  case ACTION_SPY_INCITE_CITY:
  case ACTION_TRADE_ROUTE:
  case ACTION_MARKETPLACE:
  case ACTION_HELP_WONDER:
  case ACTION_SPY_BRIBE_UNIT:
  case ACTION_SPY_SABOTAGE_UNIT:
  case ACTION_CAPTURE_UNITS:
  case ACTION_FOUND_CITY:
  case ACTION_STEAL_MAPS:
  case ACTION_SPY_NUKE:
  case ACTION_NUKE:
  case ACTION_DESTROY_CITY:
  case ACTION_EXPEL_UNIT:
  case ACTION_RECYCLE_UNIT:
  case ACTION_DISBAND_UNIT:
  case ACTION_HOME_CITY:
  case ACTION_PARADROP:
  case ACTION_AIRLIFT:
  case ACTION_CONQUER_CITY:
  case ACTION_HEAL_UNIT:
    /* No hard unit type requirements. */
    break;

  case ACTION_COUNT:
    fc_assert_ret_val(wanted_action != ACTION_COUNT, FALSE);
    break;
  }

  return TRUE;
}

/**************************************************************************
  Returns TRUE iff the wanted action is possible as far as the actor is
  concerned given that an action enabler later will enable it. Will, unlike
  action_actor_utype_hard_reqs_ok(), check the actor unit's current state.

  Can return maybe when not omniscient. Should always return yes or no when
  omniscient.
**************************************************************************/
static enum fc_tristate
action_hard_reqs_actor(const enum gen_action wanted_action,
                       const struct player *actor_player,
                       const struct city *actor_city,
                       const struct impr_type *actor_building,
                       const struct tile *actor_tile,
                       const struct unit *actor_unit,
                       const struct unit_type *actor_unittype,
                       const struct output_type *actor_output,
                       const struct specialist *actor_specialist,
                       const bool omniscient,
                       const struct city *homecity)
{
  if (!action_actor_utype_hard_reqs_ok(wanted_action, actor_unittype)) {
    /* Info leak: The actor player knows the type of his unit. */
    /* The actor unit type can't perform the action because of hard
     * unit type requirements. */
    return TRI_NO;
  }

  switch (wanted_action) {
  case ACTION_TRADE_ROUTE:
  case ACTION_MARKETPLACE:
    /* It isn't possible to establish a trade route from a non existing
     * city. The Freeciv code assumes this applies to Enter Marketplace
     * too. */
    /* Info leak: The actor player knowns his unit's home city. */
    if (homecity == NULL) {
      return TRI_NO;
    }

    break;

  case ACTION_PARADROP:
    /* Reason: Keep the old rules. */
    /* Info leak: The player knows if his unit already has paradropped this
     * turn. */
    if (actor_unit->paradropped) {
      return TRI_NO;
    }

    /* Reason: Support the paratroopers_mr_req unit type field. */
    /* Info leak: The player knows how many move fragments his unit has
     * left. */
    if (actor_unit->moves_left < actor_unittype->paratroopers_mr_req) {
      return TRI_NO;
    }

    break;

  case ACTION_AIRLIFT:
    {
      const struct city *psrc_city = tile_city(actor_tile);

      if (psrc_city == NULL) {
        /* No city to airlift from. */
        return TRI_NO;
      }

      if (actor_player != city_owner(psrc_city)
          && !(game.info.airlifting_style & AIRLIFTING_ALLIED_SRC
               && pplayers_allied(actor_player, city_owner(psrc_city)))) {
        /* Not allowed to airlift from this source. */
        return TRI_NO;
      }

      if (!(omniscient || city_owner(psrc_city) == actor_player)) {
        /* Can't check for airlifting capacity. */
        return TRI_MAYBE;
      }

      if (0 >= psrc_city->airlift) {
        /* The source cannot airlift for this turn (maybe already airlifted
         * or no airport).
         *
         * Note that (game.info.airlifting_style & AIRLIFTING_UNLIMITED_SRC)
         * is not handled here because it always needs an airport to airlift.
         * See also do_airline() in server/unittools.h. */
        return TRI_NO;
      }
    }
    break;

  case ACTION_ESTABLISH_EMBASSY:
  case ACTION_ESTABLISH_EMBASSY_STAY:
  case ACTION_SPY_INVESTIGATE_CITY:
  case ACTION_INV_CITY_SPEND:
  case ACTION_SPY_POISON:
  case ACTION_SPY_STEAL_GOLD:
  case ACTION_SPY_SABOTAGE_CITY:
  case ACTION_SPY_TARGETED_SABOTAGE_CITY:
  case ACTION_SPY_STEAL_TECH:
  case ACTION_SPY_TARGETED_STEAL_TECH:
  case ACTION_SPY_INCITE_CITY:
  case ACTION_HELP_WONDER:
  case ACTION_SPY_BRIBE_UNIT:
  case ACTION_SPY_SABOTAGE_UNIT:
  case ACTION_CAPTURE_UNITS:
  case ACTION_FOUND_CITY:
  case ACTION_JOIN_CITY:
  case ACTION_STEAL_MAPS:
  case ACTION_BOMBARD:
  case ACTION_SPY_NUKE:
  case ACTION_NUKE:
  case ACTION_DESTROY_CITY:
  case ACTION_EXPEL_UNIT:
  case ACTION_RECYCLE_UNIT:
  case ACTION_DISBAND_UNIT:
  case ACTION_HOME_CITY:
  case ACTION_UPGRADE_UNIT:
  case ACTION_ATTACK:
  case ACTION_CONQUER_CITY:
  case ACTION_HEAL_UNIT:
    /* No hard unit type requirements. */
    break;

  case ACTION_COUNT:
    fc_assert_ret_val(wanted_action != ACTION_COUNT, TRI_NO);
    break;
  }

  return TRI_YES;
}

/**************************************************************************
  Returns if the wanted action is possible given that an action enabler
  later will enable it.

  Can return maybe when not omniscient. Should always return yes or no when
  omniscient.

  This is done by checking the action's hard requirements. Hard
  requirements must be fulfilled before an action can be done. The reason
  why is usually that code dealing with the action assumes that the
  requirements are true. A requirement may also end up here if it can't be
  expressed in a requirement vector or if its absence makes the action
  pointless.

  When adding a new hard requirement here:
   * explain why it is a hard requirement in a comment.
   * remember that this is called from action_prob(). Should information
     the player don't have access to be used in a test it must check if
     the evaluation can see the thing being tested.
**************************************************************************/
static enum fc_tristate
is_action_possible(const enum gen_action wanted_action,
                   const struct player *actor_player,
                   const struct city *actor_city,
                   const struct impr_type *actor_building,
                   const struct tile *actor_tile,
                   const struct unit *actor_unit,
                   const struct unit_type *actor_unittype,
                   const struct output_type *actor_output,
                   const struct specialist *actor_specialist,
                   const struct player *target_player,
                   const struct city *target_city,
                   const struct impr_type *target_building,
                   const struct tile *target_tile,
                   const struct unit *target_unit,
                   const struct unit_type *target_unittype,
                   const struct output_type *target_output,
                   const struct specialist *target_specialist,
                   const bool omniscient,
                   const struct city *homecity,
                   bool ignore_dist)
{
  bool can_see_tgt_unit;
  bool can_see_tgt_tile;
  enum fc_tristate out;

  fc_assert_msg((action_id_get_target_kind(wanted_action) == ATK_CITY
                 && target_city != NULL)
                || (action_id_get_target_kind(wanted_action) == ATK_TILE
                    && target_tile != NULL)
                || (action_id_get_target_kind(wanted_action) == ATK_UNIT
                    && target_unit != NULL)
                || (action_id_get_target_kind(wanted_action) == ATK_UNITS
                    /* At this level each individual unit is tested. */
                    && target_unit != NULL)
                || (action_id_get_target_kind(wanted_action) == ATK_SELF),
                "Missing target!");

  /* Only check requirement against the target unit if the actor can see it
   * or if the evaluator is omniscient. The game checking the rules is
   * omniscient. The player asking about his odds isn't. */
  can_see_tgt_unit = (omniscient || (target_unit
                                     && can_player_see_unit(actor_player,
                                                            target_unit)));

  /* Only check requirement against the target tile if the actor can see it
   * or if the evaluator is omniscient. The game checking the rules is
   * omniscient. The player asking about his odds isn't. */
  can_see_tgt_tile = (omniscient
                      || plr_sees_tile(actor_player, target_tile));

  /* Info leak: The player knows where his unit is. */
  if (!ignore_dist && action_id_get_target_kind(wanted_action) != ATK_SELF
      && !action_id_distance_accepted(wanted_action,
                                      real_map_distance(actor_tile,
                                                        target_tile))) {
    /* The distance between the actor and the target isn't inside the
     * action's accepted range. */
    return TRI_NO;
  }

  switch (action_id_get_target_kind(wanted_action)) {
  case ATK_UNIT:
    /* The Freeciv code for all actions that is controlled by action
     * enablers and targets a unit assumes that the acting
     * player can see the target unit.
     * Examples:
     * - action_prob_vs_unit()'s quick check that the distance between actor
     *   and target is acceptable would leak distance to target unit if the
     *   target unit can't be seen.
     */
    if (!can_player_see_unit(actor_player, target_unit)) {
      return TRI_NO;
    }
    break;
  case ATK_CITY:
    /* The Freeciv code assumes that the player is aware of the target
     * city's existence. (How can you order an airlift to a city when you
     * don't know its city ID?) */
    if (fc_funcs->player_tile_city_id_get(city_tile(target_city),
                                          actor_player)
        != target_city->id) {
      return TRI_NO;
    }
    break;
  case ATK_UNITS:
  case ATK_TILE:
  case ATK_SELF:
    /* No special player knowledge checks. */
    break;
  case ATK_COUNT:
    fc_assert(action_id_get_target_kind(wanted_action) != ATK_COUNT);
    break;
  }

  if (action_is_blocked_by(wanted_action, actor_unit,
                           target_tile, target_city, target_unit)) {
    /* Allows an action to block an other action. If a blocking action is
     * legal the actions it blocks becomes illegal. */
    return TRI_NO;
  }

  /* Actor specific hard requirements. */
  out = action_hard_reqs_actor(wanted_action,
                               actor_player, actor_city, actor_building,
                               actor_tile, actor_unit, actor_unittype,
                               actor_output, actor_specialist,
                               omniscient, homecity);

  if (out == TRI_NO) {
    /* Illegal because of a hard actor requirement. */
    return TRI_NO;
  }

  /* Hard requirements for individual actions. */
  switch (wanted_action) {
  case ACTION_CAPTURE_UNITS:
  case ACTION_SPY_BRIBE_UNIT:
    /* Why this is a hard requirement: Can't transfer a unique unit if the
     * actor player already has one. */
    /* Info leak: This is only checked for when the actor player can see
     * the target unit. Since the target unit is seen its type is known.
     * The fact that a city hiding the unseen unit is occupied is known. */

    if (!can_see_tgt_unit) {
      /* An omniscient player can see the target unit. */
      fc_assert(!omniscient);

      return TRI_MAYBE;
    }

    if (utype_player_already_has_this_unique(actor_player,
                                             target_unittype)) {
      return TRI_NO;
    }

    /* FIXME: Capture Unit may want to look for more than one unique unit
     * of the same kind at the target tile. Currently caught by sanity
     * check in do_capture_units(). */

    break;

  case ACTION_ESTABLISH_EMBASSY:
  case ACTION_ESTABLISH_EMBASSY_STAY:
    /* Why this is a hard requirement: There is currently no point in
     * establishing an embassy when a real embassy already exists.
     * (Possible exception: crazy hack using the Lua callback
     * action_started_callback() to make establish embassy do something
     * else even if the UI still call the action Establish Embassy) */
    /* Info leak: The actor player known who he has a real embassy to. */
    if (player_has_real_embassy(actor_player, target_player)) {
      return TRI_NO;
    }

    break;

  case ACTION_SPY_TARGETED_STEAL_TECH:
    /* Reason: The Freeciv code don't support selecting a target tech
     * unless it is known that the victim player has it. */
    /* Info leak: The actor player knowns who's techs he can see. */
    if (!can_see_techs_of_target(actor_player, target_player)) {
      return TRI_NO;
    }

    break;

  case ACTION_SPY_STEAL_GOLD:
    /* If actor_unit can do the action the actor_player can see how much
     * gold target_player have. Not requireing it is therefore pointless.
     */
    if (target_player->economic.gold <= 0) {
      return TRI_NO;
    }

    break;

  case ACTION_TRADE_ROUTE:
  case ACTION_MARKETPLACE:
    {
      /* Checked in action_hard_reqs_actor() */
      fc_assert_ret_val(homecity != NULL, TRI_NO);

      /* Can't establish a trade route or enter the market place if the
       * cities can't trade at all. */
      /* TODO: Should this restriction (and the above restriction that the
       * actor unit must have a home city) be kept for Enter Marketplace? */
      if (!can_cities_trade(homecity, target_city)) {
        return TRI_NO;
      }

      /* There are more restrictions on establishing a trade route than on
       * entering the market place. */
      if (wanted_action == ACTION_TRADE_ROUTE
          && !can_establish_trade_route(homecity, target_city)) {
        return TRI_NO;
      }
    }

    break;

  case ACTION_HELP_WONDER:
  case ACTION_RECYCLE_UNIT:
    /* It is only possible to help the production if the production needs
     * the help. (If not it would be possible to add shields for something
     * that can't legally receive help if it is build later) */
    /* Info leak: The player knows that the production in his own city has
     * been hurried (bought or helped). The information isn't revealed when
     * asking for action probabilities since omniscient is FALSE. */
    if (!omniscient
        && !can_player_see_city_internals(actor_player, target_city)) {
      return TRI_MAYBE;
    }

    if (!(target_city->shield_stock
          < city_production_build_shield_cost(target_city))) {
      return TRI_NO;
    }

    break;

  case ACTION_FOUND_CITY:
    if (game.scenario.prevent_new_cities) {
      /* Reason: allow scenarios to disable city founding. */
      /* Info leak: the setting is public knowledge. */
      return TRI_NO;
    }

    if (can_see_tgt_tile && tile_city(target_tile)) {
      /* Reason: a tile can have 0 or 1 cities. */
      return TRI_NO;
    }

    switch (city_build_here_test(target_tile, actor_unit)) {
    case CB_OK:
      /* If the player knows this is checked below. */
      break;
    case CB_BAD_CITY_TERRAIN:
    case CB_BAD_UNIT_TERRAIN:
    case CB_BAD_BORDERS:
      if (can_see_tgt_tile) {
        /* Known to be blocked. Target tile is seen. */
        return TRI_NO;
      }
      break;
    case CB_NO_MIN_DIST:
      if (omniscient) {
        /* No need to check again. */
        return TRI_NO;
      } else {
        square_iterate(target_tile, game.info.citymindist - 1, otile) {
          if (tile_city(otile) != NULL
              && plr_sees_tile(actor_player, otile)) {
            /* Known to be blocked by citymindist */
            return TRI_NO;
          }
        } square_iterate_end;
      }
      break;
    }

    /* The player may not have enough information to be certain. */

    if (!can_see_tgt_tile) {
      /* Need to know if target tile already has a city, has TER_NO_CITIES
       * terrain, is non native to the actor or is owned by a foreigner. */
      return TRI_MAYBE;
    }

    if (!omniscient) {
      /* The player may not have enough information to find out if
       * citymindist blocks or not. This doesn't depend on if it blocks. */
      square_iterate(target_tile, game.info.citymindist - 1, otile) {
        if (!plr_sees_tile(actor_player, otile)) {
          /* Could have a city that blocks via citymindist. Even if this
           * tile has TER_NO_CITIES terrain the player don't know that it
           * didn't change and had a city built on it. */
          return TRI_MAYBE;
        }
      } square_iterate_end;
    }

    break;

  case ACTION_JOIN_CITY:
    {
      int new_pop;

      if (!omniscient
          && !player_can_see_city_externals(actor_player, target_city)) {
        return TRI_MAYBE;
      }

      new_pop = city_size_get(target_city) + unit_pop_value(actor_unit);

      if (new_pop > game.info.add_to_size_limit) {
        /* Reason: Make the add_to_size_limit setting work. */
        return TRI_NO;
      }

      if (!city_can_grow_to(target_city, new_pop)) {
        /* Reason: respect city size limits. */
        /* Info leak: when it is legal to join a foreign city is legal and
         * the EFT_SIZE_UNLIMIT effect or the EFT_SIZE_ADJ effect depends on
         * something the actor player don't have access to.
         * Example: depends on a building (like Aqueduct) that isn't
         * VisibleByOthers. */
        return TRI_NO;
      }
    }

    break;

  case ACTION_BOMBARD:
    /* FIXME: Target of Bombard should be city and units. */
    if (tile_city(target_tile)
        && !pplayers_at_war(city_owner(tile_city(target_tile)),
                            actor_player)) {
      return TRI_NO;
    }

    break;

  case ACTION_NUKE:
    if (actor_tile != target_tile) {
      /* The old rules only restricted other tiles. Keep them for now. */

      struct city *tcity;

      if (actor_unit->moves_left <= 0) {
        return TRI_NO;
      }

      if (!(tcity = tile_city(target_tile))
          && !unit_list_size(target_tile->units)) {
        return TRI_NO;
      }

      if (tcity && !pplayers_at_war(city_owner(tcity), actor_player)) {
        return TRI_NO;
      }

      if (is_non_attack_unit_tile(target_tile, actor_player)) {
        return TRI_NO;
      }

      if (!tcity
          && (unit_attack_units_at_tile_result(actor_unit, target_tile)
              != ATT_OK)) {
        return TRI_NO;
      }
    }

    break;

  case ACTION_HOME_CITY:
    /* Reason: can't change to what is. */
    /* Info leak: The player knows his unit's current home city. */
    if (homecity != NULL && homecity->id == target_city->id) {
      /* This is already the unit's home city. */
      return TRI_NO;
    }

    {
      int slots = unit_type_get(actor_unit)->city_slots;

      if (slots > 0 && city_unit_slots_available(target_city) < slots) {
        return TRI_NO;
      }
    }

    break;

  case ACTION_UPGRADE_UNIT:
    /* Reason: Keep the old rules. */
    /* Info leak: The player knows his unit's type. He knows if he can
     * build the unit type upgraded to. If the upgrade happens in a foreign
     * city that fact may leak. This can be seen as a price for changing
     * the rules to allow upgrading in a foreign city.
     * The player knows how much gold he has. If the Upgrade_Price_Pct
     * effect depends on information he don't have that information may
     * leak. The player knows the location of his unit. He knows if the
     * tile has a city and if the unit can exist there outside a transport.
     * The player knows his unit's cargo. By knowing their number and type
     * he can predict if there will be room for them in the unit upgraded
     * to as long as he knows what unit type his unit will end up as. */
    if (unit_upgrade_test(actor_unit, FALSE) != UU_OK) {
      return TRI_NO;
    }

    break;

  case ACTION_PARADROP:
    /* Reason: Keep the old rules. */
    /* Info leak: The player knows if he knows the target tile. */
    if (!plr_knows_tile(actor_player, target_tile)) {
      return TRI_NO;
    }

    /* Reason: Keep paratroopers_range working. */
    /* Info leak: The player knows the location of the actor and of the
     * target tile. */
    if (!ignore_dist
        && (unit_type_get(actor_unit)->paratroopers_range
            < real_map_distance(actor_tile, target_tile))) {
      return TRI_NO;
    }

    break;

  case ACTION_AIRLIFT:
    /* Reason: Keep the old rules. */
    /* Info leak: same as test_unit_can_airlift_to() */
    switch (test_unit_can_airlift_to(omniscient ? NULL : actor_player,
                                     actor_unit, target_city)) {
    case AR_OK:
      return TRI_YES;
    case AR_OK_SRC_UNKNOWN:
    case AR_OK_DST_UNKNOWN:
      return TRI_MAYBE;
    case AR_NO_MOVES:
    case AR_WRONG_UNITTYPE:
    case AR_OCCUPIED:
    case AR_NOT_IN_CITY:
    case AR_BAD_SRC_CITY:
    case AR_BAD_DST_CITY:
    case AR_SRC_NO_FLIGHTS:
    case AR_DST_NO_FLIGHTS:
      return TRI_NO;
    }

    break;

  case ACTION_ATTACK:
    /* Reason: must have a unit to attack. */
    if (unit_list_size(target_tile->units) <= 0) {
      return TRI_NO;
    }

    /* Reason: Keep the old rules. */
    if (!can_unit_attack_tile(actor_unit, target_tile)) {
      return TRI_NO;
    }
    break;

  case ACTION_CONQUER_CITY:
    /* Reason: "Conquer City" involves moving into the city. */
    if (!unit_can_move_to_tile(actor_unit, target_tile, FALSE, TRUE)) {
      return TRI_NO;
    }

    break;

  case ACTION_HEAL_UNIT:
    /* Reason: It is not the healthy who need a doctor, but the sick. */
    /* Info leak: the actor can see the target's HP. */
    if (!(target_unit->hp < target_unittype->hp)) {
      return TRI_NO;
    }
    break;

  case ACTION_SPY_INVESTIGATE_CITY:
  case ACTION_INV_CITY_SPEND:
  case ACTION_SPY_POISON:
  case ACTION_SPY_SABOTAGE_CITY:
  case ACTION_SPY_TARGETED_SABOTAGE_CITY:
  case ACTION_SPY_STEAL_TECH:
  case ACTION_SPY_INCITE_CITY:
  case ACTION_SPY_SABOTAGE_UNIT:
  case ACTION_STEAL_MAPS:
  case ACTION_SPY_NUKE:
  case ACTION_DESTROY_CITY:
  case ACTION_EXPEL_UNIT:
  case ACTION_DISBAND_UNIT:
    /* No known hard coded requirements. */
    break;
  case ACTION_COUNT:
    fc_assert(action_id_exists(wanted_action));
    break;
  }

  return out;
}

/**************************************************************************
  Return TRUE iff the action enabler is active
**************************************************************************/
static bool is_enabler_active(const struct action_enabler *enabler,
			      const struct player *actor_player,
			      const struct city *actor_city,
			      const struct impr_type *actor_building,
			      const struct tile *actor_tile,
                              const struct unit *actor_unit,
			      const struct unit_type *actor_unittype,
			      const struct output_type *actor_output,
			      const struct specialist *actor_specialist,
			      const struct player *target_player,
			      const struct city *target_city,
			      const struct impr_type *target_building,
			      const struct tile *target_tile,
                              const struct unit *target_unit,
			      const struct unit_type *target_unittype,
			      const struct output_type *target_output,
			      const struct specialist *target_specialist)
{
  return are_reqs_active(actor_player, target_player, actor_city,
                         actor_building, actor_tile,
                         actor_unit, actor_unittype,
                         actor_output, actor_specialist, NULL,
                         &enabler->actor_reqs, RPT_CERTAIN)
      && are_reqs_active(target_player, actor_player, target_city,
                         target_building, target_tile,
                         target_unit, target_unittype,
                         target_output, target_specialist, NULL,
                         &enabler->target_reqs, RPT_CERTAIN);
}

/**************************************************************************
  Returns TRUE if the wanted action is enabled.

  Note that the action may disable it self because of hard requirements
  even if an action enabler returns TRUE.
**************************************************************************/
static bool is_action_enabled(const enum gen_action wanted_action,
			      const struct player *actor_player,
			      const struct city *actor_city,
			      const struct impr_type *actor_building,
			      const struct tile *actor_tile,
                              const struct unit *actor_unit,
			      const struct unit_type *actor_unittype,
			      const struct output_type *actor_output,
			      const struct specialist *actor_specialist,
			      const struct player *target_player,
			      const struct city *target_city,
			      const struct impr_type *target_building,
			      const struct tile *target_tile,
                              const struct unit *target_unit,
			      const struct unit_type *target_unittype,
			      const struct output_type *target_output,
			      const struct specialist *target_specialist,
                              const struct city *homecity, bool ignore_dist)
{
  enum fc_tristate possible;

  possible = is_action_possible(wanted_action,
                                actor_player, actor_city,
                                actor_building, actor_tile,
                                actor_unit, actor_unittype,
                                actor_output, actor_specialist,
                                target_player, target_city,
                                target_building, target_tile,
                                target_unit, target_unittype,
                                target_output, target_specialist,
                                TRUE, homecity, ignore_dist);

  if (possible != TRI_YES) {
    /* This context is omniscient. Should be yes or no. */
    fc_assert_msg(possible != TRI_MAYBE,
                  "Is omniscient, should get yes or no.");

    /* The action enablers are irrelevant since the action it self is
     * impossible. */
    return FALSE;
  }

  action_enabler_list_iterate(action_enablers_for_action(wanted_action),
                              enabler) {
    if (is_enabler_active(enabler, actor_player, actor_city,
                          actor_building, actor_tile,
                          actor_unit, actor_unittype,
                          actor_output, actor_specialist,
                          target_player, target_city,
                          target_building, target_tile,
                          target_unit, target_unittype,
                          target_output, target_specialist)) {
      return TRUE;
    }
  } action_enabler_list_iterate_end;

  return FALSE;
}

/**************************************************************************
  Returns TRUE if actor_unit can do wanted_action to target_city as far as
  action enablers are concerned.

  See note in is_action_enabled for why the action may still be disabled.
**************************************************************************/
bool is_action_enabled_unit_on_city(const enum gen_action wanted_action,
                                    const struct unit *actor_unit,
                                    const struct city *target_city)
{
  return is_action_enabled_unit_on_city_full(wanted_action, actor_unit,
                                             target_city,
                                             game_city_by_number(actor_unit->homecity),
                                             FALSE);
}

/**************************************************************************
  Returns TRUE if actor_unit can do wanted_action to target_city as far as
  action enablers are concerned.

  See note in is_action_enabled for why the action may still be disabled.
**************************************************************************/
bool is_action_enabled_unit_on_city_full(const enum gen_action wanted_action,
                                         const struct unit *actor_unit,
                                         const struct city *target_city,
                                         const struct city *homecity,
                                         bool ignore_dist)
{
  struct tile *actor_tile = unit_tile(actor_unit);
  struct impr_type *target_building;
  struct unit_type *target_utype;

  if (actor_unit == NULL || target_city == NULL) {
    /* Can't do an action when actor or target are missing. */
    return FALSE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(wanted_action),
                        FALSE, "Action %s is performed by %s not %s",
                        gen_action_name(wanted_action),
                        action_actor_kind_name(
                          action_id_get_actor_kind(wanted_action)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_CITY
                        == action_id_get_target_kind(wanted_action),
                        FALSE, "Action %s is against %s not %s",
                        gen_action_name(wanted_action),
                        action_target_kind_name(
                          action_id_get_target_kind(wanted_action)),
                        action_target_kind_name(ATK_CITY));

  if (!unit_can_do_action(actor_unit, wanted_action)) {
    /* No point in continuing. */
    return FALSE;
  }

  target_building = tgt_city_local_building(target_city);
  target_utype = tgt_city_local_utype(target_city);

  return is_action_enabled(wanted_action,
                           unit_owner(actor_unit), tile_city(actor_tile),
                           NULL, actor_tile,
                           actor_unit, unit_type_get(actor_unit),
                           NULL, NULL,
                           city_owner(target_city), target_city,
                           target_building, city_tile(target_city),
                           NULL, target_utype, NULL, NULL, homecity,
                           ignore_dist);
}

/**************************************************************************
  Returns TRUE if actor_unit can do wanted_action to target_unit as far as
  action enablers are concerned.

  See note in is_action_enabled for why the action may still be disabled.
**************************************************************************/
bool is_action_enabled_unit_on_unit(const enum gen_action wanted_action,
                                    const struct unit *actor_unit,
                                    const struct unit *target_unit)
{
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL || target_unit == NULL) {
    /* Can't do an action when actor or target are missing. */
    return FALSE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(wanted_action),
                        FALSE, "Action %s is performed by %s not %s",
                        gen_action_name(wanted_action),
                        action_actor_kind_name(
                          action_id_get_actor_kind(wanted_action)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_UNIT
                        == action_id_get_target_kind(wanted_action),
                        FALSE, "Action %s is against %s not %s",
                        gen_action_name(wanted_action),
                        action_target_kind_name(
                          action_id_get_target_kind(wanted_action)),
                        action_target_kind_name(ATK_UNIT));

  if (!unit_can_do_action(actor_unit, wanted_action)) {
    /* No point in continuing. */
    return FALSE;
  }

  return is_action_enabled(wanted_action,
                           unit_owner(actor_unit), tile_city(actor_tile),
                           NULL, actor_tile,
                           actor_unit, unit_type_get(actor_unit),
                           NULL, NULL,
                           unit_owner(target_unit),
                           tile_city(unit_tile(target_unit)), NULL,
                           unit_tile(target_unit),
                           target_unit, unit_type_get(target_unit),
                           NULL, NULL,
                           game_city_by_number(actor_unit->homecity),
                           FALSE);
}

/**************************************************************************
  Returns TRUE if actor_unit can do wanted_action to all units on the
  target_tile as far as action enablers are concerned.

  See note in is_action_enabled for why the action may still be disabled.
**************************************************************************/
bool is_action_enabled_unit_on_units(const enum gen_action wanted_action,
                                     const struct unit *actor_unit,
                                     const struct tile *target_tile)
{
  struct tile *actor_tile = unit_tile(actor_unit);
  struct city *homecity;

  if (actor_unit == NULL || target_tile == NULL
      || unit_list_size(target_tile->units) == 0) {
    /* Can't do an action when actor or target are missing. */
    return FALSE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(wanted_action),
                        FALSE, "Action %s is performed by %s not %s",
                        gen_action_name(wanted_action),
                        action_actor_kind_name(
                          action_id_get_actor_kind(wanted_action)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_UNITS
                        == action_id_get_target_kind(wanted_action),
                        FALSE, "Action %s is against %s not %s",
                        gen_action_name(wanted_action),
                        action_target_kind_name(
                          action_id_get_target_kind(wanted_action)),
                        action_target_kind_name(ATK_UNITS));

  if (!unit_can_do_action(actor_unit, wanted_action)) {
    /* No point in continuing. */
    return FALSE;
  }

  homecity = game_city_by_number(actor_unit->homecity);

  unit_list_iterate(target_tile->units, target_unit) {
    if (!is_action_enabled(wanted_action,
                           unit_owner(actor_unit), tile_city(actor_tile),
                           NULL, actor_tile,
                           actor_unit, unit_type_get(actor_unit),
                           NULL, NULL,
                           unit_owner(target_unit),
                           tile_city(unit_tile(target_unit)), NULL,
                           unit_tile(target_unit),
                           target_unit, unit_type_get(target_unit),
                           NULL, NULL, homecity, FALSE)) {
      /* One unit makes it impossible for all units. */
      return FALSE;
    }
  } unit_list_iterate_end;

  /* Not impossible for any of the units at the tile. */
  return TRUE;
}

/**************************************************************************
  Returns TRUE if actor_unit can do wanted_action to the target_tile as far
  as action enablers are concerned.

  See note in is_action_enabled for why the action may still be disabled.
**************************************************************************/
bool is_action_enabled_unit_on_tile(const enum gen_action wanted_action,
                                    const struct unit *actor_unit,
                                    const struct tile *target_tile)
{
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL || target_tile == NULL) {
    /* Can't do an action when actor or target are missing. */
    return FALSE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(wanted_action),
                        FALSE, "Action %s is performed by %s not %s",
                        gen_action_name(wanted_action),
                        action_actor_kind_name(
                          action_id_get_actor_kind(wanted_action)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_TILE
                        == action_id_get_target_kind(wanted_action),
                        FALSE, "Action %s is against %s not %s",
                        gen_action_name(wanted_action),
                        action_target_kind_name(
                          action_id_get_target_kind(wanted_action)),
                        action_target_kind_name(ATK_TILE));

  if (!unit_can_do_action(actor_unit, wanted_action)) {
    /* No point in continuing. */
    return FALSE;
  }

  return is_action_enabled(wanted_action,
                           unit_owner(actor_unit), tile_city(actor_tile),
                           NULL, actor_tile,
                           actor_unit, unit_type_get(actor_unit),
                           NULL, NULL,
                           tile_owner(target_tile), NULL, NULL,
                           target_tile, NULL, NULL, NULL, NULL,
                           game_city_by_number(actor_unit->homecity),
                           FALSE);
}

/**************************************************************************
  Returns TRUE if actor_unit can do wanted_action to itself as far as
  action enablers are concerned.

  See note in is_action_enabled() for why the action still may be
  disabled.
**************************************************************************/
bool is_action_enabled_unit_on_self(const enum gen_action wanted_action,
                                    const struct unit *actor_unit)
{
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL) {
    /* Can't do an action when the actor is missing. */
    return FALSE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(wanted_action),
                        FALSE, "Action %s is performed by %s not %s",
                        gen_action_name(wanted_action),
                        action_actor_kind_name(
                          action_id_get_actor_kind(wanted_action)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_SELF
                        == action_id_get_target_kind(wanted_action),
                        FALSE, "Action %s is against %s not %s",
                        gen_action_name(wanted_action),
                        action_target_kind_name(
                          action_id_get_target_kind(wanted_action)),
                        action_target_kind_name(ATK_SELF));

  if (!unit_can_do_action(actor_unit, wanted_action)) {
    /* No point in continuing. */
    return FALSE;
  }

  return is_action_enabled(wanted_action,
                           unit_owner(actor_unit), tile_city(actor_tile),
                           NULL, actor_tile,
                           actor_unit, unit_type_get(actor_unit),
                           NULL, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                           game_city_by_number(actor_unit->homecity), FALSE);
}

/**************************************************************************
  Find out if the action is enabled, may be enabled or isn't enabled given
  what the player owning the actor knowns.

  A player don't always know everything needed to figure out if an action
  is enabled or not. A server side AI with the same limits on its knowledge
  as a human player or a client should use this to figure out what is what.

  Assumes to be called from the point of view of the actor. Its knowledge
  is assumed to be given in the parameters.

  Returns TRI_YES if the action is enabled, TRI_NO if it isn't and
  TRI_MAYBE if the player don't know enough to tell.

  If meta knowledge is missing TRI_MAYBE will be returned.
**************************************************************************/
static enum fc_tristate
action_enabled_local(const enum gen_action wanted_action,
                     const struct player *actor_player,
                     const struct city *actor_city,
                     const struct impr_type *actor_building,
                     const struct tile *actor_tile,
                     const struct unit *actor_unit,
                     const struct output_type *actor_output,
                     const struct specialist *actor_specialist,
                     const struct player *target_player,
                     const struct city *target_city,
                     const struct impr_type *target_building,
                     const struct tile *target_tile,
                     const struct unit *target_unit,
                     const struct output_type *target_output,
                     const struct specialist *target_specialist)
{
  enum fc_tristate current;
  enum fc_tristate result;

  result = TRI_NO;
  action_enabler_list_iterate(action_enablers_for_action(wanted_action),
                              enabler) {
    current = fc_tristate_and(mke_eval_reqs(actor_player, actor_player,
                                            target_player, actor_city,
                                            actor_building, actor_tile,
                                            actor_unit, actor_output,
                                            actor_specialist,
                                            &enabler->actor_reqs,
                                            RPT_CERTAIN),
                              mke_eval_reqs(actor_player, target_player,
                                            actor_player, target_city,
                                            target_building, target_tile,
                                            target_unit, target_output,
                                            target_specialist,
                                            &enabler->target_reqs,
                                            RPT_CERTAIN));
    if (current == TRI_YES) {
      return TRI_YES;
    } else if (current == TRI_MAYBE) {
      result = TRI_MAYBE;
    }
  } action_enabler_list_iterate_end;

  return result;
}

/**************************************************************************
  Find out if the effect value is known

  The knowledge of the actor is assumed to be given in the parameters.

  If meta knowledge is missing TRI_MAYBE will be returned.
**************************************************************************/
static bool is_effect_val_known(enum effect_type effect_type,
                                const struct player *pow_player,
                                const struct player *target_player,
                                const struct player *other_player,
                                const struct city *target_city,
                                const struct impr_type *target_building,
                                const struct tile *target_tile,
                                const struct unit *target_unit,
                                const struct output_type *target_output,
                                const struct specialist *target_specialist)
{
  effect_list_iterate(get_effects(effect_type), peffect) {
    if (TRI_MAYBE == mke_eval_reqs(pow_player, target_player,
                                   other_player, target_city,
                                   target_building, target_tile,
                                   target_unit, target_output,
                                   target_specialist,
                                   &(peffect->reqs), RPT_CERTAIN)) {
      return FALSE;
    }
  } effect_list_iterate_end;

  return TRUE;
}

/**************************************************************************
  Does the target has any techs the actor don't?
**************************************************************************/
static enum fc_tristate
tech_can_be_stolen(const struct player *actor_player,
                   const struct player *target_player)
{
  const struct research *actor_research = research_get(actor_player);
  const struct research *target_research = research_get(target_player);

  if (actor_research != target_research) {
    if (can_see_techs_of_target(actor_player, target_player)) {
      advance_iterate(A_FIRST, padvance) {
        Tech_type_id i = advance_number(padvance);

        if (research_invention_state(target_research, i) == TECH_KNOWN
            && research_invention_gettable(actor_research, i,
                                           game.info.tech_steal_allow_holes)
            && (research_invention_state(actor_research, i) == TECH_UNKNOWN
                || (research_invention_state(actor_research, i)
                    == TECH_PREREQS_KNOWN))) {
          return TRI_YES;
        }
      } advance_iterate_end;
    } else {
      return TRI_MAYBE;
    }
  }

  return TRI_NO;
}

/**************************************************************************
  The action probability that pattacker will win a diplomatic battle.

  It is assumed that pattacker and pdefender have different owners and that
  the defender can defend in a diplomatic battle.

  See diplomat_success_vs_defender() in server/diplomats.c
**************************************************************************/
static struct act_prob ap_dipl_battle_win(const struct unit *pattacker,
                                          const struct unit *pdefender)
{
  /* Keep unconverted until the end to avoid scaling each step */
  int chance;
  struct act_prob out;

  /* Superspy always win */
  if (unit_has_type_flag(pdefender, UTYF_SUPERSPY)) {
    /* A defending UTYF_SUPERSPY will defeat every possible attacker. */
    return ACTPROB_IMPOSSIBLE;
  }
  if (unit_has_type_flag(pattacker, UTYF_SUPERSPY)) {
    /* An attacking UTYF_SUPERSPY will defeat every possible defender
     * except another UTYF_SUPERSPY. */
    return ACTPROB_CERTAIN;
  }

  /* Base chance is 50% */
  chance = 50;

  /* Spy attack bonus */
  if (unit_has_type_flag(pattacker, UTYF_SPY)) {
    chance += 25;
  }

  /* Spy defense bonus */
  if (unit_has_type_flag(pdefender, UTYF_SPY)) {
    chance -= 25;
  }

  /* Veteran attack and defense bonus */
  {
    const struct veteran_level *vatt =
        utype_veteran_level(unit_type_get(pattacker), pattacker->veteran);
    const struct veteran_level *vdef =
        utype_veteran_level(unit_type_get(pdefender), pdefender->veteran);

    chance += vatt->power_fact - vdef->power_fact;
  }

  /* Defense bonus. */
  {
    if (!is_effect_val_known(EFT_SPY_RESISTANT, unit_owner(pattacker),
                             tile_owner(pdefender->tile),  NULL,
                             tile_city(pdefender->tile), NULL,
                             pdefender->tile, NULL, NULL, NULL)) {
      return ACTPROB_NOT_KNOWN;
    }

    /* Reduce the chance of an attack by EFT_SPY_RESISTANT percent. */
    chance -= chance
              * get_target_bonus_effects(NULL,
                                         tile_owner(pdefender->tile), NULL,
                                         tile_city(pdefender->tile), NULL,
                                         pdefender->tile, NULL, NULL, NULL,
                                         NULL, NULL,
                                         EFT_SPY_RESISTANT) / 100;
  }

  /* Convert to action probability */
  out.min = chance * ACTPROB_VAL_1_PCT;
  out.max = chance * ACTPROB_VAL_1_PCT;

  return out;
}

/**************************************************************************
  The action probability that pattacker will win a diplomatic battle.

  See diplomat_infiltrate_tile() in server/diplomats.c
**************************************************************************/
static struct act_prob ap_diplomat_battle(const struct unit *pattacker,
                                          const struct unit *pvictim)
{
  unit_list_iterate(unit_tile(pvictim)->units, punit) {
    if (unit_owner(punit) == unit_owner(pattacker)) {
      /* Won't defend against its owner. */
      continue;
    }

    if (punit == pvictim
        && !unit_has_type_flag(punit, UTYF_SUPERSPY)) {
      /* The victim unit is defenseless unless it's a SuperSpy.
       * Rationalization: A regular diplomat don't mind being bribed. A
       * SuperSpy is high enough up the chain that accepting a bribe is
       * against his own interests. */
      continue;
    }

    if (!(unit_has_type_flag(punit, UTYF_DIPLOMAT)
        || unit_has_type_flag(punit, UTYF_SUPERSPY))) {
      /* The unit can't defend. */
      continue;
    }

    /* There will be a diplomatic battle in stead of an action. */
    return ap_dipl_battle_win(pattacker, punit);
  } unit_list_iterate_end;

  /* No diplomatic battle will occur. */
  return ACTPROB_CERTAIN;
}

/***************************************************************************
  Returns the action probability for when a target is unseen.
***************************************************************************/
static struct act_prob act_prob_unseen_target(int action_id,
                                              const struct unit *actor_unit)
{
  if (action_maybe_possible_actor_unit(action_id, actor_unit)) {
    /* Unknown because the target is unseen. */
    return ACTPROB_NOT_KNOWN;
  } else {
    /* The actor it self can't do this. */
    return ACTPROB_IMPOSSIBLE;
  }
}

/**************************************************************************
  An action's probability of success.

  "Success" indicates that the action achieves its goal, not that the
  actor survives. For actions that cost money it is assumed that the
  player has and is willing to spend the money. This is so the player can
  figure out what his odds are before deciding to get the extra money.
**************************************************************************/
static struct act_prob
action_prob(const enum gen_action wanted_action,
            const struct player *actor_player,
            const struct city *actor_city,
            const struct impr_type *actor_building,
            const struct tile *actor_tile,
            const struct unit *actor_unit,
            const struct unit_type *actor_unittype_p,
            const struct output_type *actor_output,
            const struct specialist *actor_specialist,
            const struct player *target_player,
            const struct city *target_city,
            const struct impr_type *target_building,
            const struct tile *target_tile,
            const struct unit *target_unit,
            const struct unit_type *target_unittype_p,
            const struct output_type *target_output,
            const struct specialist *target_specialist)
{
  int known;
  struct act_prob chance;
  const struct unit_type *actor_unittype;
  const struct unit_type *target_unittype;
  const struct city *homecity;

  if (actor_unittype_p == NULL && actor_unit != NULL) {
    actor_unittype = unit_type_get(actor_unit);
  } else {
    actor_unittype = actor_unittype_p;
  }

  if (target_unittype_p == NULL && target_unit != NULL) {
    target_unittype = unit_type_get(target_unit);
  } else {
    target_unittype = target_unittype_p;
  }

  if (actor_unit != NULL) {
    homecity = game_city_by_number(actor_unit->homecity);
  } else {
    homecity = NULL;
  }

  known = is_action_possible(wanted_action,
                             actor_player, actor_city,
                             actor_building, actor_tile,
                             actor_unit, actor_unittype,
                             actor_output, actor_specialist,
                             target_player, target_city,
                             target_building, target_tile,
                             target_unit, target_unittype,
                             target_output, target_specialist,
                             FALSE, homecity, FALSE);

  if (known == TRI_NO) {
    /* The action enablers are irrelevant since the action it self is
     * impossible. */
    return ACTPROB_IMPOSSIBLE;
  }

  chance = ACTPROB_NOT_IMPLEMENTED;

  known = fc_tristate_and(known,
                          action_enabled_local(wanted_action,
                                               actor_player, actor_city,
                                               actor_building, actor_tile,
                                               actor_unit,
                                               actor_output,
                                               actor_specialist,
                                               target_player, target_city,
                                               target_building, target_tile,
                                               target_unit,
                                               target_output,
                                               target_specialist));

  switch (wanted_action) {
  case ACTION_SPY_POISON:
    /* TODO */
    break;
  case ACTION_SPY_STEAL_GOLD:
    /* TODO */
    break;
  case ACTION_STEAL_MAPS:
    /* TODO */
    break;
  case ACTION_SPY_SABOTAGE_UNIT:
    /* All uncertainty comes from potential diplomatic battles. */
    chance = ap_diplomat_battle(actor_unit, target_unit);
    break;
  case ACTION_SPY_BRIBE_UNIT:
    /* All uncertainty comes from potential diplomatic battles. */
    chance = ap_diplomat_battle(actor_unit, target_unit);;
    break;
  case ACTION_SPY_SABOTAGE_CITY:
    /* TODO */
    break;
  case ACTION_SPY_TARGETED_SABOTAGE_CITY:
    /* TODO */
    break;
  case ACTION_SPY_INCITE_CITY:
    /* TODO */
    break;
  case ACTION_ESTABLISH_EMBASSY:
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_ESTABLISH_EMBASSY_STAY:
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_SPY_STEAL_TECH:
    /* Do the victim have anything worth taking? */
    known = fc_tristate_and(known,
                            tech_can_be_stolen(actor_player,
                                               target_player));

    /* TODO: Calculate actual chance */

    break;
  case ACTION_SPY_TARGETED_STEAL_TECH:
    /* Do the victim have anything worth taking? */
    known = fc_tristate_and(known,
                            tech_can_be_stolen(actor_player,
                                               target_player));

    /* TODO: Calculate actual chance */

    break;
  case ACTION_SPY_INVESTIGATE_CITY:
    /* There is no risk that the city won't get investigated. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_INV_CITY_SPEND:
    /* There is no risk that the city won't get investigated. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_TRADE_ROUTE:
    /* TODO */
    break;
  case ACTION_MARKETPLACE:
    /* Possible when not blocked by is_action_possible() */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_HELP_WONDER:
    /* Possible when not blocked by is_action_possible() */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_CAPTURE_UNITS:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_EXPEL_UNIT:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_BOMBARD:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_FOUND_CITY:
    /* Possible when not blocked by is_action_possible() */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_JOIN_CITY:
    /* Possible when not blocked by is_action_possible() */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_SPY_NUKE:
    /* TODO */
    break;
  case ACTION_NUKE:
    /* TODO */
    break;
  case ACTION_DESTROY_CITY:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_RECYCLE_UNIT:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_DISBAND_UNIT:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_HOME_CITY:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_UPGRADE_UNIT:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_PARADROP:
    /* TODO */
    break;
  case ACTION_AIRLIFT:
    /* TODO */
    break;
  case ACTION_ATTACK:
    {
      struct unit *defender_unit = get_defender(actor_unit, target_tile);

      if (can_player_see_unit(actor_player, defender_unit)) {
        double unconverted = unit_win_chance(actor_unit, defender_unit);

        chance.min = MAX(ACTPROB_VAL_MIN,
                         floor((double)ACTPROB_VAL_MAX * unconverted));
        chance.max = MIN(ACTPROB_VAL_MAX,
                         ceil((double)ACTPROB_VAL_MAX * unconverted));
      } else if (known == TRI_YES) {
        known = TRI_MAYBE;
      }
    }
    break;
  case ACTION_CONQUER_CITY:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_HEAL_UNIT:
    /* No battle is fought first. */
    chance = ACTPROB_CERTAIN;
    break;
  case ACTION_COUNT:
    fc_assert(wanted_action != ACTION_COUNT);
    break;
  }

  /* Non signal action probabilities should be in range. */
  fc_assert_action((action_prob_is_signal(chance)
                    || chance.max <= ACTPROB_VAL_MAX),
                   chance.max = ACTPROB_VAL_MAX);
  fc_assert_action((action_prob_is_signal(chance)
                    || chance.min >= ACTPROB_VAL_MIN),
                   chance.min = ACTPROB_VAL_MIN);

  switch (known) {
  case TRI_NO:
    return ACTPROB_IMPOSSIBLE;
    break;
  case TRI_MAYBE:
    return ACTPROB_NOT_KNOWN;
    break;
  case TRI_YES:
    return chance;
    break;
  };

  fc_assert_ret_val_msg(FALSE, ACTPROB_NOT_IMPLEMENTED,
                        "Should be yes, maybe or no");
}

/**************************************************************************
  Get the actor unit's probability of successfully performing the chosen
  action on the target city.
**************************************************************************/
struct act_prob action_prob_vs_city(const struct unit* actor_unit,
                                    const int action_id,
                                    const struct city* target_city)
{
  struct tile *actor_tile = unit_tile(actor_unit);
  struct impr_type *target_building;
  struct unit_type *target_utype;

  if (actor_unit == NULL || target_city == NULL) {
    /* Can't do an action when actor or target are missing. */
    return ACTPROB_IMPOSSIBLE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is performed by %s not %s",
                        gen_action_name(action_id),
                        action_actor_kind_name(
                          action_id_get_actor_kind(action_id)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_CITY == action_id_get_target_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is against %s not %s",
                        gen_action_name(action_id),
                        action_target_kind_name(
                          action_id_get_target_kind(action_id)),
                        action_target_kind_name(ATK_CITY));

  if (!unit_can_do_action(actor_unit, action_id)) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* Doesn't leak information about city position since an unknown city
   * can't be targeted and a city can't move. */
  if (!action_id_distance_accepted(action_id,
          real_map_distance(unit_tile(actor_unit),
                            city_tile(target_city)))) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  if (!player_can_see_city_externals(unit_owner(actor_unit), target_city)) {
    /* The invisible city at this tile may, as far as the player knows, not
     * exist anymore. */
    return act_prob_unseen_target(action_id, actor_unit);
  }

  target_building = tgt_city_local_building(target_city);
  target_utype = tgt_city_local_utype(target_city);

  return action_prob(action_id,
                     unit_owner(actor_unit), tile_city(actor_tile),
                     NULL, actor_tile, actor_unit, NULL,
                     NULL, NULL,
                     city_owner(target_city), target_city,
                     target_building, city_tile(target_city),
                     NULL, target_utype, NULL, NULL);
}

/**************************************************************************
  Get the actor unit's probability of successfully performing the chosen
  action on the target unit.
**************************************************************************/
struct act_prob action_prob_vs_unit(const struct unit* actor_unit,
                                    const int action_id,
                                    const struct unit* target_unit)
{
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL || target_unit == NULL) {
    /* Can't do an action when actor or target are missing. */
    return ACTPROB_IMPOSSIBLE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is performed by %s not %s",
                        gen_action_name(action_id),
                        action_actor_kind_name(
                          action_id_get_actor_kind(action_id)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_UNIT == action_id_get_target_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is against %s not %s",
                        gen_action_name(action_id),
                        action_target_kind_name(
                          action_id_get_target_kind(action_id)),
                        action_target_kind_name(ATK_UNIT));

  if (!unit_can_do_action(actor_unit, action_id)) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* Doesn't leak information about unit position since an unseen unit can't
   * be targeted. */
  if (!action_id_distance_accepted(action_id,
          real_map_distance(unit_tile(actor_unit),
                            unit_tile(target_unit)))) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  return action_prob(action_id,
                     unit_owner(actor_unit), tile_city(actor_tile),
                     NULL, actor_tile, actor_unit, NULL,
                     NULL, NULL,
                     unit_owner(target_unit),
                     tile_city(unit_tile(target_unit)), NULL,
                     unit_tile(target_unit),
                     target_unit, NULL, NULL, NULL);
}

/**************************************************************************
  Get the actor unit's probability of successfully performing the chosen
  action on all units at the target tile.
**************************************************************************/
struct act_prob action_prob_vs_units(const struct unit* actor_unit,
                                     const int action_id,
                                     const struct tile* target_tile)
{
  struct act_prob prob_all;
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL || target_tile == NULL) {
    /* Can't do an action when actor or target are missing. */
    return ACTPROB_IMPOSSIBLE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is performed by %s not %s",
                        gen_action_name(action_id),
                        action_actor_kind_name(
                          action_id_get_actor_kind(action_id)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_UNITS == action_id_get_target_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is against %s not %s",
                        gen_action_name(action_id),
                        action_target_kind_name(
                          action_id_get_target_kind(action_id)),
                        action_target_kind_name(ATK_UNITS));

  if (!unit_can_do_action(actor_unit, action_id)) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* Doesn't leak information about unit stack position since it is
   * specified as a tile and an unknown tile's position is known. */
  if (!action_id_distance_accepted(action_id,
                                   real_map_distance(unit_tile(actor_unit),
                                                     target_tile))) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* Doesn't leak information since the actor player can see the target
   * tile. */
  if (tile_is_seen(target_tile, unit_owner(actor_unit))
      && tile_city(target_tile) != NULL
      && !utype_can_do_act_if_tgt_citytile(unit_type_get(actor_unit),
                                           action_id,
                                           CITYT_CENTER, TRUE)) {
    /* Don't offer to perform actions that never can target a unit stack in
     * a city. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* Must be done here since an empty unseen tile will result in
   * ACTPROB_IMPOSSIBLE. */
  if (unit_list_size(target_tile->units) == 0) {
    /* Can't act against an empty tile. */

    if (player_can_trust_tile_has_no_units(unit_owner(actor_unit),
                                           target_tile)) {
      /* Known empty tile. */
      return ACTPROB_IMPOSSIBLE;
    } else {
      /* The player doesn't know that the tile is empty. */
      return act_prob_unseen_target(action_id, actor_unit);
    }
  }

  /* Invisible units at this tile can make the action legal or illegal.
   * Invisible units can be stacked with visible units. The possible
   * existence of invisible units therefore makes the result uncertain. */
  prob_all = (can_player_see_hypotetic_units_at(unit_owner(actor_unit),
                                                target_tile)
              ? ACTPROB_CERTAIN : ACTPROB_NOT_KNOWN);

  unit_list_iterate(target_tile->units, target_unit) {
    struct act_prob prob_unit;

    if (!can_player_see_unit(unit_owner(actor_unit), target_unit)) {
      /* Only visible units are considered. The invisible units contributed
       * their uncertainty to prob_all above. */
      continue;
    }

    prob_unit = action_prob(action_id,
                            unit_owner(actor_unit),
                            tile_city(actor_tile),
                            NULL, actor_tile, actor_unit, NULL,
                            NULL, NULL,
                            unit_owner(target_unit),
                            tile_city(unit_tile(target_unit)), NULL,
                            unit_tile(target_unit),
                            target_unit, NULL,
                            NULL, NULL);

    if (!action_prob_possible(prob_unit)) {
      /* One unit makes it impossible for all units. */
      return ACTPROB_IMPOSSIBLE;
    } else if (action_prob_not_impl(prob_unit)) {
      /* Not implemented dominates all except impossible. */
      prob_all = ACTPROB_NOT_IMPLEMENTED;
    } else {
      fc_assert_msg(!action_prob_is_signal(prob_unit),
                    "Invalid probability [%d, %d]",
                    prob_unit.min, prob_unit.max);

      if (action_prob_is_signal(prob_all)) {
        /* Special values dominate regular values. */
        continue;
      }

      /* Probability against all target units considered until this moment
       * and the probability against this target unit. */
      prob_all.min = (prob_all.min * prob_unit.min) / ACTPROB_VAL_MAX;
      prob_all.max = (prob_all.max * prob_unit.max) / ACTPROB_VAL_MAX;
      break;
    }
  } unit_list_iterate_end;

  /* Not impossible for any of the units at the tile. */
  return prob_all;
}

/**************************************************************************
  Get the actor unit's probability of successfully performing the chosen
  action on the target tile.
**************************************************************************/
struct act_prob action_prob_vs_tile(const struct unit* actor_unit,
                                    const int action_id,
                                    const struct tile* target_tile)
{
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL || target_tile == NULL) {
    /* Can't do an action when actor or target are missing. */
    return ACTPROB_IMPOSSIBLE;
  }

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is performed by %s not %s",
                        gen_action_name(action_id),
                        action_actor_kind_name(
                          action_id_get_actor_kind(action_id)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_TILE == action_id_get_target_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is against %s not %s",
                        gen_action_name(action_id),
                        action_target_kind_name(
                          action_id_get_target_kind(action_id)),
                        action_target_kind_name(ATK_TILE));

  if (!unit_can_do_action(actor_unit, action_id)) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* Doesn't leak information about tile position since an unknown tile's
   * position is known. */
  if (!action_id_distance_accepted(action_id,
                                   real_map_distance(unit_tile(actor_unit),
                                                     target_tile))) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  return action_prob(action_id,
                     unit_owner(actor_unit), tile_city(actor_tile),
                     NULL, actor_tile, actor_unit, NULL,
                     NULL, NULL,
                     tile_owner(target_tile), NULL, NULL,
                     target_tile, NULL, NULL, NULL, NULL);
}

/**************************************************************************
  Get the actor unit's probability of successfully performing the chosen
  action on itself.
**************************************************************************/
struct act_prob action_prob_self(const struct unit* actor_unit,
                                 const int action_id)
{
  struct tile *actor_tile = unit_tile(actor_unit);

  if (actor_unit == NULL) {
    /* Can't do the action when the actor is missing. */
    return ACTPROB_IMPOSSIBLE;
  }

  /* No point in checking distance to target. It is always 0. */

  fc_assert_ret_val_msg(AAK_UNIT == action_id_get_actor_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is performed by %s not %s",
                        gen_action_name(action_id),
                        action_actor_kind_name(
                          action_id_get_actor_kind(action_id)),
                        action_actor_kind_name(AAK_UNIT));

  fc_assert_ret_val_msg(ATK_SELF == action_id_get_target_kind(action_id),
                        ACTPROB_IMPOSSIBLE,
                        "Action %s is against %s not %s",
                        gen_action_name(action_id),
                        action_target_kind_name(
                          action_id_get_target_kind(action_id)),
                        action_target_kind_name(ATK_SELF));

  if (!unit_can_do_action(actor_unit, action_id)) {
    /* No point in continuing. */
    return ACTPROB_IMPOSSIBLE;
  }

  return action_prob(action_id,
                     unit_owner(actor_unit), tile_city(actor_tile),
                     NULL, actor_tile, actor_unit, NULL,
                     NULL, NULL,
                     NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

/**************************************************************************
  Returns the impossible action probability.
**************************************************************************/
struct act_prob action_prob_new_impossible(void)
{
  struct act_prob out = { ACTPROB_VAL_MIN, ACTPROB_VAL_MIN };

  return out;
}

/**************************************************************************
  Returns the certain action probability.
**************************************************************************/
struct act_prob action_prob_new_certain(void)
{
  struct act_prob out = { ACTPROB_VAL_MAX, ACTPROB_VAL_MAX };

  return out;
}

/**************************************************************************
  Returns the n/a action probability.
**************************************************************************/
struct act_prob action_prob_new_not_relevant(void)
{
  struct act_prob out = { ACTPROB_VAL_NA, ACTPROB_VAL_MIN};

  return out;
}

/**************************************************************************
  Returns the "not implemented" action probability.
**************************************************************************/
struct act_prob action_prob_new_not_impl(void)
{
  struct act_prob out = { ACTPROB_VAL_NOT_IMPL, ACTPROB_VAL_MIN };

  return out;
}

/**************************************************************************
  Returns the "user don't know" action probability.
**************************************************************************/
struct act_prob action_prob_new_unknown(void)
{
  struct act_prob out = { ACTPROB_VAL_MIN, ACTPROB_VAL_MAX };

  return out;
}

/**************************************************************************
  Returns TRUE iff the given action probability belongs to an action that
  may be possible.
**************************************************************************/
bool action_prob_possible(const struct act_prob probability)
{
  return (ACTPROB_VAL_MIN < probability.max
          || action_prob_not_impl(probability));
}

/**************************************************************************
  Returns TRUE iff the given action probability represents the lack of
  an action probability.
**************************************************************************/
static inline bool
action_prob_not_relevant(const struct act_prob probability)
{
  return probability.min == ACTPROB_VAL_NA
         && probability.max == ACTPROB_VAL_MIN;
}

/**************************************************************************
  Returns TRUE iff the given action probability represents that support
  for finding this action probability currently is missing from Freeciv.
**************************************************************************/
static inline bool
action_prob_not_impl(const struct act_prob probability)
{
  return probability.min == ACTPROB_VAL_NOT_IMPL
         && probability.max == ACTPROB_VAL_MIN;
}

/**************************************************************************
  Returns TRUE iff the given action probability represents a special
  signal value rather than a regular action probability value.
**************************************************************************/
static inline bool
action_prob_is_signal(const struct act_prob probability)
{
  return probability.max < probability.min;
}

/**************************************************************************
  Returns TRUE iff ap1 and ap2 are equal.
**************************************************************************/
bool are_action_probabilitys_equal(const struct act_prob *ap1,
                                   const struct act_prob *ap2)
{
  return ap1->min == ap2->min && ap1->max == ap2->max;
}

/**************************************************************************
  Compare action probabilities. Prioritize the lowest possible value.
**************************************************************************/
int action_prob_cmp_pessimist(const struct act_prob ap1,
                              const struct act_prob ap2)
{
  struct act_prob my_ap1;
  struct act_prob my_ap2;

  /* The action probabilities are real. */
  fc_assert(!action_prob_not_relevant(ap1));
  fc_assert(!action_prob_not_relevant(ap2));

  /* Convert any signals to ACTPROB_NOT_KNOWN. */
  if (action_prob_is_signal(ap1)) {
    /* Assert that it is OK to convert the signal. */
    fc_assert(action_prob_not_impl(ap1));

    my_ap1 = ACTPROB_NOT_KNOWN;
  } else {
    my_ap1 = ap1;
  }

  if (action_prob_is_signal(ap2)) {
    /* Assert that it is OK to convert the signal. */
    fc_assert(action_prob_not_impl(ap2));

    my_ap2 = ACTPROB_NOT_KNOWN;
  } else {
    my_ap2 = ap2;
  }

  /* The action probabilities now have a comparison friendly form. */
  fc_assert(!action_prob_is_signal(my_ap1));
  fc_assert(!action_prob_is_signal(my_ap2));

  /* Do the comparison. Start with min. Continue with max. */
  if (my_ap1.min < my_ap2.min) {
    return -1;
  } else if (my_ap1.min > my_ap2.min) {
    return 1;
  } else if (my_ap1.max < my_ap2.max) {
    return -1;
  } else if (my_ap1.max > my_ap2.max) {
    return 1;
  } else {
    return 0;
  }
}

/**************************************************************************
  Returns double in the range [0-1] representing the minimum of the given
  action probability.
**************************************************************************/
double action_prob_to_0_to_1_pessimist(const struct act_prob ap)
{
  struct act_prob my_ap;

  /* The action probability is real. */
  fc_assert(!action_prob_not_relevant(ap));

  /* Convert any signals to ACTPROB_NOT_KNOWN. */
  if (action_prob_is_signal(ap)) {
    /* Assert that it is OK to convert the signal. */
    fc_assert(action_prob_not_impl(ap));

    my_ap = ACTPROB_NOT_KNOWN;
  } else {
    my_ap = ap;
  }

  /* The action probability now has a math friendly form. */
  fc_assert(!action_prob_is_signal(my_ap));

  return (double)my_ap.min / (double) ACTPROB_VAL_MAX;
}

/**************************************************************************
  Returns ap1 with ap2 as fall back in cases where ap1 doesn't happen.
  Said in math that is: P(A) + P(A') * P(B)

  This is useful to calculate the probability of doing action A or, when A
  is impossible, falling back to doing action B.
**************************************************************************/
struct act_prob action_prob_fall_back(const struct act_prob *ap1,
                                      const struct act_prob *ap2)
{
  struct act_prob my_ap1;
  struct act_prob my_ap2;
  struct act_prob out;

  /* The action probabilities are real. */
  fc_assert(ap1 && !action_prob_not_relevant(*ap1));
  fc_assert(ap2 && !action_prob_not_relevant(*ap2));

  if (action_prob_is_signal(*ap1)
      && are_action_probabilitys_equal(ap1, ap2)) {
    /* Keep the information rather than converting the signal to
     * ACTPROB_NOT_KNOWN. */

    /* Assert that it is OK to convert the signal. */
    fc_assert(action_prob_not_impl(*ap1));

    out.min = ap1->min;
    out.max = ap2->max;

    return out;
  }

  /* Convert any signals to ACTPROB_NOT_KNOWN. */
  if (action_prob_is_signal(*ap1)) {
    /* Assert that it is OK to convert the signal. */
    fc_assert(action_prob_not_impl(*ap1));

    my_ap1.min = ACTPROB_VAL_MIN;
    my_ap1.max = ACTPROB_VAL_MAX;
  } else {
    my_ap1.min = ap1->min;
    my_ap1.max = ap1->max;
  }

  if (action_prob_is_signal(*ap2)) {
    /* Assert that it is OK to convert the signal. */
    fc_assert(action_prob_not_impl(*ap2));

    my_ap2.min = ACTPROB_VAL_MIN;
    my_ap2.max = ACTPROB_VAL_MAX;
  } else {
    my_ap2.min = ap2->min;
    my_ap2.max = ap2->max;
  }

  /* The action probabilities now have a math friendly form. */
  fc_assert(!action_prob_is_signal(my_ap1));
  fc_assert(!action_prob_is_signal(my_ap2));

  /* Do the math. */
  out.min = my_ap1.min + (((ACTPROB_VAL_MAX - my_ap1.min) * my_ap2.min)
                          / ACTPROB_VAL_MAX);
  out.max = my_ap1.max + (((ACTPROB_VAL_MAX - my_ap1.max) * my_ap2.max)
                          / ACTPROB_VAL_MAX);

  return out;
}

/**************************************************************************
  Will a player with the government gov be immune to the action act?
**************************************************************************/
bool action_immune_government(struct government *gov, int act)
{
  /* Always immune since its not enabled. Doesn't count. */
  if (action_enabler_list_size(action_enablers_for_action(act)) == 0) {
    return FALSE;
  }

  action_enabler_list_iterate(action_enablers_for_action(act), enabler) {
    if (requirement_fulfilled_by_government(gov, &(enabler->target_reqs))) {
      return FALSE;
    }
  } action_enabler_list_iterate_end;

  return TRUE;
}

/**************************************************************************
  Returns TRUE if the specified action never can be performed when the
  situation requirement is fulfilled for the actor.
**************************************************************************/
bool action_blocked_by_situation_act(const struct action *paction,
                                     const struct requirement *situation)
{
  action_enabler_list_iterate(action_enablers_for_action(paction->id),
                              enabler) {
    if (!does_req_contradicts_reqs(situation, &enabler->actor_reqs)) {
      return FALSE;
    }
  } action_enabler_list_iterate_end;

  return TRUE;
}

/**************************************************************************
  Returns TRUE if the specified action never can be performed when the
  situation requirement is fulfilled for the target.
**************************************************************************/
bool action_blocked_by_situation_tgt(const struct action *paction,
                                     const struct requirement *situation)
{
  action_enabler_list_iterate(action_enablers_for_action(paction->id),
                              enabler) {
    if (!does_req_contradicts_reqs(situation, &enabler->target_reqs)) {
      return FALSE;
    }
  } action_enabler_list_iterate_end;

  return TRUE;
}

/**************************************************************************
  Returns TRUE if the wanted action can be done to the target.
**************************************************************************/
static bool is_target_possible(const enum gen_action wanted_action,
			       const struct player *actor_player,
			       const struct player *target_player,
			       const struct city *target_city,
			       const struct impr_type *target_building,
			       const struct tile *target_tile,
                               const struct unit *target_unit,
			       const struct unit_type *target_unittype,
			       const struct output_type *target_output,
			       const struct specialist *target_specialist)
{
  action_enabler_list_iterate(action_enablers_for_action(wanted_action),
                              enabler) {
    if (are_reqs_active(target_player, actor_player, target_city,
                        target_building, target_tile,
                        target_unit, target_unittype,
                        target_output, target_specialist, NULL,
                        &enabler->target_reqs, RPT_POSSIBLE)) {
      return TRUE;
    }
  } action_enabler_list_iterate_end;

  return FALSE;
}

/**************************************************************************
  Returns TRUE if the wanted action can be done to the target city.
**************************************************************************/
bool is_action_possible_on_city(const enum gen_action action_id,
                                const struct player *actor_player,
                                const struct city* target_city)
{
  fc_assert_ret_val_msg(ATK_CITY == action_id_get_target_kind(action_id),
                        FALSE, "Action %s is against %s not cities",
                        gen_action_name(action_id),
                        action_target_kind_name(
                          action_id_get_target_kind(action_id)));

  return is_target_possible(action_id, actor_player,
                            city_owner(target_city), target_city, NULL,
                            city_tile(target_city), NULL, NULL,
                            NULL, NULL);
}

/**************************************************************************
  Returns TRUE if the wanted action (as far as the player knows) can be
  performed right now by the specified actor unit if an approriate target
  is provided.
**************************************************************************/
bool action_maybe_possible_actor_unit(const int action_id,
                                      const struct unit *actor_unit)
{
  const struct player *actor_player = unit_owner(actor_unit);
  const struct tile *actor_tile = unit_tile(actor_unit);
  const struct city *actor_city = tile_city(actor_tile);
  const struct unit_type *actor_unittype = unit_type_get(actor_unit);

  enum fc_tristate result;

  fc_assert_ret_val(actor_unit, FALSE);

  if (!utype_can_do_action(actor_unit->utype, action_id)) {
    /* The unit type can't perform the action. */
    return FALSE;
  }

  result = action_hard_reqs_actor(action_id,
                                  actor_player, actor_city, NULL,
                                  actor_tile, actor_unit, actor_unittype,
                                  NULL, NULL, FALSE,
                                  game_city_by_number(actor_unit->homecity));

  if (result == TRI_NO) {
    /* The hard requirements aren't fulfilled. */
    return FALSE;
  }

  action_enabler_list_iterate(action_enablers_for_action(action_id),
                              enabler) {
    const enum fc_tristate current
        = mke_eval_reqs(actor_player,
                        actor_player, NULL, actor_city, NULL, actor_tile,
                        actor_unit, NULL, NULL,
                        &enabler->actor_reqs,
                        /* Needed since no player to evaluate DiplRel
                         * requirements against. */
                        RPT_POSSIBLE);

    if (current == TRI_YES
        || current == TRI_MAYBE) {
      /* The ruleset requirements may be fulfilled. */
      return TRUE;
    }
  } action_enabler_list_iterate_end;

  /* No action enabler allows this action. */
  return FALSE;
}

/**************************************************************************
  Returns TRUE if the specified action can't be done now but would have
  been legal if the unit had full movement.
**************************************************************************/
bool action_mp_full_makes_legal(const struct unit *actor,
                                const int action_id)
{
  fc_assert(action_id_exists(action_id) || action_id == ACTION_ANY);

  /* Check if full movement points may enable the specified action. */
  return !utype_may_act_move_frags(unit_type_get(actor),
                                   action_id,
                                   actor->moves_left)
      && utype_may_act_move_frags(unit_type_get(actor),
                                  action_id,
                                  unit_move_rate(actor));
}

/**************************************************************************
  Returns action auto performer rule slot number num so it can be filled.
**************************************************************************/
struct action_auto_perf *action_auto_perf_slot_number(const int num)
{
  fc_assert_ret_val(num >= 0, NULL);
  fc_assert_ret_val(num < MAX_NUM_ACTION_AUTO_PERFORMERS, NULL);

  return &auto_perfs[num];
}

/**************************************************************************
  Returns action auto performer rule number num.

  Used in action_auto_perf_iterate()

  WARNING: If the cause of the returned action performer rule is
  AAPC_COUNT it means that it is unused.
**************************************************************************/
const struct action_auto_perf *action_auto_perf_by_number(const int num)
{
  return action_auto_perf_slot_number(num);
}