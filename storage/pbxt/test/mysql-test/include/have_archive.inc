--disable_query_log
--require r/true.require
select support = 'Enabled' as `TRUE` from information_schema.engines where engine = 'archive';
--enable_query_log