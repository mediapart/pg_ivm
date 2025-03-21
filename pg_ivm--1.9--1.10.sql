-- functions

CREATE FUNCTION create_immv(text, text, boolean default true)
RETURNS bigint
STRICT
AS 'MODULE_PATHNAME', 'create_immv'
LANGUAGE C;

DROP FUNCTION create_immv(text, text);
