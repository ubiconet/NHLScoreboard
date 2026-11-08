#pragma once

// Connection-progress screen, shown by the shell while the network service
// is connecting (power-on, a lost link, setup mode's retry of the saved
// network, or credentials just saved in the portal): the network being
// joined, the step the radio is on, and how long until the board gives up
// and opens setup mode. One full paint per attempt (~1 s on this
// software-SPI panel); after that a small Wi-Fi icon animates and the
// status block repaints once a second, so the render loop stays responsive.
void renderConnectScreen();

// Something else painted over the screen (a portal override): the next
// renderConnectScreen() call repaints in full.
void invalidateConnectScreen();
