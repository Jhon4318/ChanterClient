#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace minecraft::container_ui {

class Listener {
public:
    virtual ~Listener() = default;
    virtual void on_container_opened(void* controller) = 0;
    virtual void on_container_tick() = 0;
    virtual void on_container_lost() = 0;
};

void set_listener(Listener* listener);

// These hooks are optional. The client remains usable when the exact game
// signatures are unavailable; ContainerTransfer simply stays unavailable.
bool initialize();
void shutdown();
bool available();

void arm_next_open();
bool complete_user_open(bool gameAccepted);
void cancel_pending_open();

bool has_hidden_screen();
void* screen_controller();
bool tick_offstack_screen();
void release_offstack_screen();

int collection_size(std::string_view collection);
const void* stack_at(std::string_view collection, int index);
bool is_empty(const void* stack);
int count_of(const void* stack);
int max_stack_of(const void* stack);
int net_tag_of(const void* stack);
std::string item_name(const void* stack);

bool shift_click(std::string_view collection, int index);
bool pending_predictions();

} // namespace minecraft::container_ui
