/*
	Copyright (C) 2010 - 2025
	by Gabriel Morin <gabrielmorin (at) gmail (dot) com>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 * @file
 */

#include "whiteboard/attack.hpp"

#include <utility>

#include "whiteboard/visitor.hpp"

#include "config.hpp"
#include "draw.hpp"
#include "fake_unit_ptr.hpp"
#include "game_board.hpp"
#include "play_controller.hpp"
#include "resources.hpp"
#include "units/unit.hpp"
#include "units/map.hpp"

namespace wb
{

std::ostream &operator<<(std::ostream &s, const attack_ptr& attack)
{
	assert(attack);
	return attack->print(s);
}

std::ostream &operator<<(std::ostream &s, const attack_const_ptr& attack)
{
	assert(attack);
	return attack->print(s);
}

std::ostream& attack::print(std::ostream& s) const
{
	s << "Attack on (" << get_target_hex() << ") preceded by ";
	move::print(s);
	return s;
}

attack::attack(std::size_t team_index, bool hidden, const unit& u, const map_location& target_hex, int weapon_choice, const pathfind::marked_route& route,
		arrow_ptr arrow, fake_unit_ptr fake_unit)
	: move(team_index, hidden, u, route, std::move(arrow), std::move(fake_unit)),
	target_hex_(target_hex),
	weapon_choice_(weapon_choice),
	attack_movement_cost_(u.attacks()[weapon_choice_].movement_used()),
	temp_movement_subtracted_(0),
	attack_count_(u.attacks()[weapon_choice_].attacks_used()),
	temp_attacks_subtracted_(0)
{
	this->init();
}

attack::attack(const config& cfg, bool hidden)
	: move(cfg,hidden)
	, target_hex_(cfg.mandatory_child("target_hex_")["x"], cfg.mandatory_child("target_hex_")["y"], wml_loc())
	, weapon_choice_(cfg["weapon_choice_"].to_int(-1)) //default value: -1
	, attack_movement_cost_()
	, temp_movement_subtracted_(0)
	, attack_count_()
	, temp_attacks_subtracted_(0)
{
	// Validate target_hex
	if(!tiles_adjacent(target_hex_,get_dest_hex()))
		throw action::ctor_err("attack: Invalid target_hex_");

	// Validate weapon_choice_
	if(weapon_choice_ < 0 || weapon_choice_ >= static_cast<int>(get_unit()->attacks().size()))
		throw action::ctor_err("attack: Invalid weapon_choice_");

	// Construct attack_movement_cost_
	assert(get_unit());
	attack_movement_cost_ = get_unit()->attacks()[weapon_choice_].movement_used();
	attack_count_ = get_unit()->attacks()[weapon_choice_].attacks_used();

	this->init();
}

void attack::init()
{
	display::get_singleton()->invalidate(target_hex_);
}

attack::~attack()
{
	invalidate();
}

void attack::accept(visitor& v)
{
	v.visit(shared_from_this());
}

/* private */
void attack::invalidate()
{
	if(display::get_singleton())
	{
		//invalidate dest and target hex so attack indicator is properly cleared
		display::get_singleton()->invalidate(get_dest_hex());
		display::get_singleton()->invalidate(target_hex_);
	}
}

void attack::execute(bool& success, bool& complete)
{
	if(!valid()) {
		success = false;
		//Setting complete to true signifies to side_actions to delete the planned action: nothing more to do with it.
		complete = true;
		return;
	}

	LOG_WB << "Executing: " << shared_from_this();

	if (route_->steps.size() >= 2)
	{
		bool m_success, m_complete;
		move::execute(m_success,m_complete);
		if(!m_success) {
			//Move failed for some reason, so don't attack.
			success = false;
			complete = true;
			return;
		}
	}

	resources::controller->get_mouse_handler_base().attack_enemy(get_dest_hex(), get_target_hex(), weapon_choice_);
	complete = true;

	//check that attacking unit is still alive, if not, consider the attack a failure
	unit_map::const_iterator survivor = resources::gameboard->units().find(get_dest_hex());
	if(!survivor.valid() || (!unit_id_.empty() && (survivor->id() != unit_id_)))
	{
		success = false;
	}

	success = true;
}

void attack::apply_temp_modifier(unit_map& unit_map)
{
	move::apply_temp_modifier(unit_map);
	assert(get_unit());
	unit& unit = *get_unit();
	DBG_WB << unit.name() << " [" << unit.id()
					<< "] has " << unit.attacks_left() << " attacks, decreasing by " << attack_count_;
	assert(unit.attacks_left() > 0);

	//Calculate movement to subtract
	temp_movement_subtracted_ = unit.movement_left() >= attack_movement_cost_ ? attack_movement_cost_ : 0 ;
	temp_attacks_subtracted_ = unit.attacks_left() >= attack_count_ ? attack_count_ : 0 ;
	DBG_WB << "Attack: Changing movement points for unit " << unit.name() << " [" << unit.id()
				<< "] from " << unit.movement_left() << " to "
				<< unit.movement_left() - temp_movement_subtracted_ << ".";
	unit.set_movement(unit.movement_left() - temp_movement_subtracted_, true);
	unit.set_attacks(unit.attacks_left() - temp_attacks_subtracted_);

	//Update status of fake unit (not undone by remove_temp_modifiers)
	//@todo this contradicts the name "temp_modifiers"
	if (fake_unit_) { //Attacks that are not attack-moves don't have fake units
		fake_unit_->set_movement(unit.movement_left(), true);
		fake_unit_->set_attacks(unit.attacks_left());
	}
}

void attack::remove_temp_modifier(unit_map& unit_map)
{
	assert(get_unit());
	unit& unit = *get_unit();
	DBG_WB << unit.name() << " [" << unit.id()
					<< "] has " << unit.attacks_left() << " attacks, increasing by one";
	unit.set_attacks(unit.attacks_left() + temp_attacks_subtracted_);
	DBG_WB << "Attack: Changing movement points for unit " << unit.name() << " [" << unit.id()
				<< "] from " << unit.movement_left() << " to "
				<< unit.movement_left() + temp_movement_subtracted_ << ".";
	unit.set_movement(unit.movement_left() + temp_movement_subtracted_, true);
	temp_movement_subtracted_ = 0;
	temp_attacks_subtracted_ = 0;
	move::remove_temp_modifier(unit_map);
}

// Draws the attack indicator.
void attack::draw_hex(const map_location& hex)
{
	if(hex != get_dest_hex() && hex != target_hex_) {
		return;
	}

	//@todo: replace this by either the use of transparency + drawing_layer::attack_indicator,
	//or a dedicated layer
	const drawing_layer layer = drawing_layer::footsteps;

	//calculate direction (valid for both hexes)
	const std::string direction_text = map_location::write_direction(get_dest_hex().get_relative_dir(target_hex_));

	texture indicator = (hex == get_dest_hex())
		? image::get_texture("whiteboard/attack-indicator-src-" + direction_text + ".png", image::HEXED)
		: image::get_texture("whiteboard/attack-indicator-dst-" + direction_text + ".png", image::HEXED);

	// hex is either the dst or target here. Whichever it is, we want to draw there.
	display::get_singleton()->drawing_buffer_add(
		layer, hex, [tex = std::move(indicator)](const rect& d) { draw::blit(tex, d); });
}

void attack::redraw()
{
	move::redraw();
	display::get_singleton()->invalidate(target_hex_);
}

action::error attack::check_validity() const
{
	// Verify that the unit that planned this attack exists
	if(!get_unit()) {
		return NO_UNIT;
	}
	// Verify that the target hex is still valid
	if(!target_hex_.valid()) {
		return INVALID_LOCATION;
	}
	// Verify that the target hex isn't empty
	if(resources::gameboard->units().find(target_hex_) == resources::gameboard->units().end()){
		return NO_TARGET;
	}
	// Verify that the attacking unit has attacks left
	if(get_unit()->attacks_left() <= 0) {
		return NO_ATTACK_LEFT;
	}
	// Verify that the attacker and target are enemies
	if(!resources::gameboard->get_team(get_unit()->side()).is_enemy(resources::gameboard->units().find(target_hex_)->side())){
		return NOT_AN_ENEMY;
	}
	//@todo: (maybe) verify that the target hex contains the same unit that before,
	// comparing for example the unit ID

	return move::check_validity();
}

config attack::to_config() const
{
	config final_cfg = move::to_config();

	final_cfg["type"] = "attack";
	final_cfg["weapon_choice_"] = weapon_choice_;
//	final_cfg["attack_movement_cost_"] = attack_movement_cost_; //Unnecessary
//	final_cfg["temp_movement_subtracted_"] = temp_movement_subtracted_; //Unnecessary

	config target_hex_cfg;
	target_hex_cfg["x"]=target_hex_.wml_x();
	target_hex_cfg["y"]=target_hex_.wml_y();
	final_cfg.add_child("target_hex_", std::move(target_hex_cfg));

	return final_cfg;
}

} // end namespace wb
