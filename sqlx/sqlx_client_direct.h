/*
OpenIO SDS sqlx
Copyright (C) 2015 OpenIO, original work as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OIO_SDS__sqlx__sqlx_client_direct_h
# define OIO_SDS__sqlx__sqlx_client_direct_h 1

# include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct oio_directory_s;

/* Creates the default SQLX client that locates then contacts sqlx servers */
struct oio_sqlx_client_factory_s * oio_sqlx_client_factory__create_sds (
		const char *ns, struct oio_directory_s *dir);

#ifdef __cplusplus
}
#endif

#endif /*OIO_SDS__sqlx__sqlx_client_direct_h*/
