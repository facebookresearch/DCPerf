INSERT OVERWRITE TABLE table_SPdBtF_Y9di_dNz1Hh3XBLWloMbgaL_el8BCohoZGlQ_
                  PARTITION (ds='2018-12-29')
                  SELECT
                  coalesce(d.`c8k3`, b.`c8k3`), coalesce(d.`cvwqz6MyB1`, b.`cvwqz6MyB1`), CAST(coalesce(d.`A3nORKj`, b.`A3nORKj`) AS bigint), CAST(coalesce(d.`RB9`, b.`RB9`) AS bigint), CAST(coalesce(d.`YYk8V71j`, b.`YYk8V71j`) AS bigint), CAST(coalesce(d.`DLqnqUwZ`, b.`DLqnqUwZ`) AS bigint), coalesce(d.`fq7ay`, b.`fq7ay`), CAST(coalesce(d.`FJuZpmHF6Y`, b.`FJuZpmHF6Y`) AS bigint), CAST(coalesce(d.`r7h`, b.`r7h`) AS bigint)
                  FROM
                  (
                      SELECT *
                      FROM table__CkHRpFW2Qb5fJVQSg91UOUPBHBj8lnRjzeTRxWzuDI_
                      WHERE ds='2018-12-29'
                  ) d
                  FULL OUTER JOIN
                  (
                      SELECT *
                      FROM table_uu9fgFJsVdtOC878DXBVyo_IqVwQVjeAOXhXDBOM8Tc_ b
                      WHERE ds='2018-12-28'
                  ) b
                  ON b.`r7h` = d.`r7h` AND b.`RB9` = d.`RB9` AND b.`FJuZpmHF6Y` = d.`FJuZpmHF6Y`
                  WHERE 
                  (coalesce(d.cvwqz6MyB1, b.cvwqz6MyB1) = 105)
               AND coalesce(d.`FJuZpmHF6Y`, b.`FJuZpmHF6Y`)  in (1137159934)

		UNION ALL
               
	       SELECT
                  coalesce(d.`c8k3`, b.`c8k3`), coalesce(d.`cvwqz6MyB1`, b.`cvwqz6MyB1`), CAST(coalesce(d.`A3nORKj`, b.`A3nORKj`) AS bigint), CAST(coalesce(d.`RB9`, b.`RB9`) AS bigint), CAST(coalesce(d.`YYk8V71j`, b.`YYk8V71j`) AS bigint), CAST(coalesce(d.`DLqnqUwZ`, b.`DLqnqUwZ`) AS bigint), coalesce(d.`fq7ay`, b.`fq7ay`), CAST(coalesce(d.`FJuZpmHF6Y`, b.`FJuZpmHF6Y`) AS bigint), CAST(coalesce(d.`r7h`, b.`r7h`) AS bigint)
                  FROM
                  (
                      SELECT *
                      FROM table__CkHRpFW2Qb5fJVQSg91UOUPBHBj8lnRjzeTRxWzuDI_
                      WHERE ds='2018-12-29'
                  ) d
                  FULL OUTER JOIN
                  (
                      SELECT *
                      FROM table_uu9fgFJsVdtOC878DXBVyo_IqVwQVjeAOXhXDBOM8Tc_ b
                      WHERE ds='2018-12-28'
                  ) b
                  ON b.`r7h` = d.`r7h` AND b.`RB9` = d.`RB9` AND b.`FJuZpmHF6Y` = d.`FJuZpmHF6Y`
                  WHERE 
                  (coalesce(d.cvwqz6MyB1, b.cvwqz6MyB1) = 127)
               AND coalesce(d.`FJuZpmHF6Y`, b.`FJuZpmHF6Y`)  in (1137159934)

		UNION ALL
               
	       SELECT
                  coalesce(d.`c8k3`, b.`c8k3`), coalesce(d.`cvwqz6MyB1`, b.`cvwqz6MyB1`), CAST(coalesce(d.`A3nORKj`, b.`A3nORKj`) AS bigint), CAST(coalesce(d.`RB9`, b.`RB9`) AS bigint), CAST(coalesce(d.`YYk8V71j`, b.`YYk8V71j`) AS bigint), CAST(coalesce(d.`DLqnqUwZ`, b.`DLqnqUwZ`) AS bigint), coalesce(d.`fq7ay`, b.`fq7ay`), CAST(coalesce(d.`FJuZpmHF6Y`, b.`FJuZpmHF6Y`) AS bigint), CAST(coalesce(d.`r7h`, b.`r7h`) AS bigint)
                  FROM
                  (
                      SELECT *
                      FROM table__CkHRpFW2Qb5fJVQSg91UOUPBHBj8lnRjzeTRxWzuDI_
                      WHERE ds='2018-12-29'
                  ) d
                  FULL OUTER JOIN
                  (
                      SELECT *
                      FROM table_uu9fgFJsVdtOC878DXBVyo_IqVwQVjeAOXhXDBOM8Tc_ b
                      WHERE ds='2018-12-28'
                  ) b
                  ON b.`r7h` = d.`r7h` AND b.`RB9` = d.`RB9` AND b.`FJuZpmHF6Y` = d.`FJuZpmHF6Y`
                  WHERE 
                  (coalesce(d.cvwqz6MyB1, b.cvwqz6MyB1) = 127)
               AND coalesce(d.`FJuZpmHF6Y`, b.`FJuZpmHF6Y`)  in (1137159934)
