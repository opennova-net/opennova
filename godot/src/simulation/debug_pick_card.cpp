#include "simulation/debug_pick_card.h"
#include "util/record_bind.h"

using namespace godot;

Ref<DebugPickCard> DebugPickCard::duplicate() const {
	Ref<DebugPickCard> out;
	out.instantiate();
#define DEBUG_PICK_CARD_COPY(m_type, m_name, m_default) out->m_name##_ = m_name##_;
	DEBUG_PICK_CARD_FIELDS(DEBUG_PICK_CARD_COPY)
#undef DEBUG_PICK_CARD_COPY
	return out;
}

void DebugPickCard::_bind_methods() {
	DEBUG_PICK_CARD_FIELDS(OPENNOVA_RECORD_FIELD)
	ClassDB::bind_method(D_METHOD("duplicate"), &DebugPickCard::duplicate);
}
