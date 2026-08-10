// blob client — SFML 3 window, input capture, snapshot rendering.
//
// Rendering stays deliberately boring: circles and a camera. The interesting
// work is the snapshot buffer and interpolation, which lands here later.

#include <blob/net/protocol.hpp>
#include <blob/net/quantize.hpp>
#include <blob/sim/world.hpp>

#include <enet/enet.h>

#include <SFML/Graphics.hpp>

#include <cstdio>
#include <optional>

namespace {

struct EnetGuard {
    EnetGuard() : ok{enet_initialize() == 0} {}
    ~EnetGuard() { if (ok) { enet_deinitialize(); } }
    EnetGuard(const EnetGuard&) = delete;
    EnetGuard& operator=(const EnetGuard&) = delete;
    bool ok;
};

} // namespace

int main()
{
    EnetGuard enet;
    if (!enet.ok) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }

    sf::RenderWindow window{sf::VideoMode{{1280u, 720u}}, "blob"};
    window.setVerticalSyncEnabled(true);

    // Stand-in for the interpolated snapshot state: one cell that follows the
    // cursor locally so there is something moving on screen before the netcode
    // exists. Delete this once snapshots drive the view.
    sf::CircleShape cell{32.0f};
    cell.setOrigin({32.0f, 32.0f});
    cell.setFillColor(sf::Color{90, 160, 240});
    cell.setPosition({640.0f, 360.0f});

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            } else if (const auto* key = event->getIf<sf::Event::KeyPressed>()) {
                if (key->code == sf::Keyboard::Key::Escape) {
                    window.close();
                }
            }
        }

        // Input -> intent. The client sends only this; never a position.
        const sf::Vector2i mouse = sf::Mouse::getPosition(window);
        const sf::Vector2f centre{static_cast<float>(window.getSize().x) * 0.5f,
                                  static_cast<float>(window.getSize().y) * 0.5f};
        const blob::math::Vec2 to_cursor{static_cast<float>(mouse.x) - centre.x,
                                         static_cast<float>(mouse.y) - centre.y};
        const blob::math::Vec2 dir = blob::math::normalized(to_cursor);

        [[maybe_unused]] const blob::net::InputCommand cmd{
            .sequence = 0,
            .dir_x = blob::net::quantize_direction(dir.x),
            .dir_y = blob::net::quantize_direction(dir.y),
            .split = false,
            .eject = false,
        };
        // TODO: pack `cmd` and send unreliably each client tick.

        window.clear(sf::Color{18, 18, 22});
        window.draw(cell);
        window.display();
    }

    return 0;
}
