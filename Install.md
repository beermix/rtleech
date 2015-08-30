## Ubuntu Package ##
Download both of the debs and install them. Do _not_ just get one. The client will not function properly if both are not there. These packages will replace existing versions of rtorrent/libtorrent so beware.

## Source Install ##
Checkout the source from subversion, see the SVN page for details.
cd into the libtorrent directory and run:

./configure && make && make install

then do the same in the rtorrent directory.