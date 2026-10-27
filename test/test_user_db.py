import pytest
from sqlalchemy import text
from common import restart_worker


def test_http_with_different_username_dbname(sess, autocommit_sess):
    """Check that a pre existing role can use the net schema"""

    try:
        autocommit_sess.execute(
            text("alter system set pg_http.username to 'pre_existing'")
        )
        autocommit_sess.execute(
            text("alter system set pg_http.database_name to 'pre_existing'")
        )
        restart_worker(autocommit_sess)

        (username, datname) = sess.execute(
            text(
                """
            select usename, datname from pg_stat_activity where backend_type ilike '%pg_http%';
        """
            )
        ).fetchone()
        assert username == "pre_existing"
        assert datname == "pre_existing"

    finally:
        autocommit_sess.execute(text("alter system reset pg_http.username"))
        autocommit_sess.execute(text("alter system reset pg_http.database_name"))
        restart_worker(autocommit_sess)


def test_http_appname(sess):
    """Check that pg_stat_activity has appname set"""

    (count,) = sess.execute(
        text(
            """
        select count(1) from pg_stat_activity where application_name like '%pg_http%';
    """
        )
    ).fetchone()
    assert count == 1
