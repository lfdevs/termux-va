TERMUX_PKG_HOMEPAGE=https://github.com/lfdevs/termux-va
TERMUX_PKG_DESCRIPTION="MediaCodec hardware decode and encode bridge for Termux"
TERMUX_PKG_LICENSE="GPL-3.0"
TERMUX_PKG_MAINTAINER="@lfdevs"
TERMUX_PKG_VERSION="0.1.0"
TERMUX_PKG_API_LEVEL=29
TERMUX_PKG_SKIP_SRC_EXTRACT=true
# Use the API-specific NDK MediaCodec stub for APIs enabled at this level.
TERMUX_PKG_BUILD_DEPENDS="ndk-multilib"

termux_step_make() {
	local repo_root
	repo_root="$(realpath "$TERMUX_PKG_BUILDER_DIR/../..")"
	local ndk_lib_dir="$TERMUX_STANDALONE_TOOLCHAIN/sysroot/usr/lib/$TERMUX_HOST_PLATFORM/$TERMUX_PKG_API_LEVEL"

	$CC $CPPFLAGS $CFLAGS -Wall -Wextra -std=c11 \
		-I"$repo_root/common" \
		"$repo_root/daemon/termux-va.c" \
		-o termux-va \
		$LDFLAGS "$ndk_lib_dir/libmediandk.so" -llog -landroid

	$CC $CPPFLAGS $CFLAGS -Wall -Wextra -std=c11 \
		-I"$repo_root/common" \
		"$repo_root/tools/tva-probe.c" \
		-o tva-probe \
		$LDFLAGS
}

termux_step_make_install() {
	local repo_root
	repo_root="$(realpath "$TERMUX_PKG_BUILDER_DIR/../..")"

	install -Dm700 termux-va "$TERMUX_PREFIX/bin/termux-va"
	install -Dm700 tva-probe "$TERMUX_PREFIX/bin/tva-probe"
	install -Dm700 "$repo_root/scripts/termux-va-start.sh" "$TERMUX_PREFIX/bin/termux-va-start"
	install -Dm700 "$repo_root/scripts/termux-va-stop.sh" "$TERMUX_PREFIX/bin/termux-va-stop"
	install -Dm700 "$repo_root/scripts/termux-va-watchdog.sh" "$TERMUX_PREFIX/bin/termux-va-watchdog"
}
