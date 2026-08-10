/* SPDX-License-Identifier: Apache-2.0 */

#include <zest/battery_monitor.hpp>
#include <zest/bluetooth_manager.hpp>
#include <zest/http_client.hpp>
#include <zest/wifi_manager.hpp>

static_assert(zest::percent_from_mv(4200) == 100);
static_assert(zest::percent_from_mv(3270) == 0);

int main()
{
	return 0;
}
