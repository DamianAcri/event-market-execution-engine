# Investigación aplicada: encontrar, dimensionar y capturar margen

Revisión base: 15 de septiembre de 2026. Actualización: 17 de septiembre de 2026. Proyecto: Event Market Execution Engine (Calci). Investigación y propuestas; las funcionalidades existentes se identifican expresamente.

**Plan vigente:** [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) mantiene el orden, estado y criterios de ejecución. Este documento conserva la investigación y sus propuestas; las extensiones solo se incorporan bajo las condiciones del plan.

## Actualización aplicada — 17 de septiembre de 2026

La captura de dos horas de `btc-20260916T183127.659551Z` registró 1.947.001 actualizaciones en ocho mercados de un evento BTC, con 28 implicaciones. El replay y la reconstrucción independiente no encontraron margen bruto positivo en esas parejas, incluso al relajar frescura y cantidades enteras; no hubo órdenes simuladas ni beneficio. El resultado limita esa muestra y política: no mide el universo de Kalshi ni demuestra que aumentar velocidad o capital hubiera creado oportunidades.

**Decisión actual, implementada y probada localmente:** ampliar observaciones antes de ajustar la estrategia. El perfil `research` selecciona hasta dos eventos BTC y 16 rangos de strike uniformemente espaciados por evento, incluidos los extremos; añade hasta 16 mercados NFL para observación. Son hasta 48 mercados estáticos, con límites operativos explícitos, no una selección que maximice ingresos. La paginación pública, los archivos originales con hashes y `coverage.json` deben hacer visibles selección, exclusiones y relaciones admitidas. La política y selección se fijan antes de capturar; las pruebas locales y un preflight autenticado de 30 segundos pasan, pero este último solo recibió los 48 libros iniciales. La recepción de trades reales y la evaluación económica representativa siguen pendientes.

La literatura determina qué hipótesis merece observarse y qué supuestos hay que comprobar. Saguillo apoya buscar relaciones y revisar su semántica; Cheng, Yang y Zou motiva observar ganador/margen y fases del evento, sin trasladar resultados NBA/Polymarket a NFL/Kalshi.[^saguillo][^nba] Los términos NFL revisados incluyen empate a 0,50 y liquidaciones excepcionales a precio justo discrecional. El empate por sí solo conserva el mínimo de ganador-YES más spread-positivo-NO —paga 1,50—, pero nuestro oráculo booleano no representa ese estado. Tampoco hemos establecido una garantía conjunta para los pagos discrecionales. **NFL queda fuera de las restricciones certificadas y de las operaciones simuladas de esta entrega**; admitirlo requiere modelar y justificar esos pagos, no solo emparejar títulos.[^nflwinner][^nflspread]

Capturar operaciones públicas junto al libro prepara la comparación de ejecución pasiva y agresiva estudiada por Cont y Kukanov; no implementa su optimizador ni acredita nuestras ejecuciones.[^cont] Huang, Lehalle y Rosenbaum explican que los cambios agregados de cola no identifican qué orden se canceló y que ciertos supuestos pueden sobreestimar fills.[^queue] Por ello los trades observados conservan su dirección, precio y condición de bloque; no alteran por sí mismos el libro ni producen fills simulados. Los bloques se negocian fuera del libro. No se atribuirá dos veces el mismo consumo a un trade y a su delta.[^trades]

El siguiente experimento pasivo solo se abre tras validar captura/replay y disponer de flujo suficiente para evaluar sus supuestos. Debe declarar cola inicial, latencia, tratamiento de cancelaciones, comisiones, selección adversa y exposición de patas; comparar con la referencia agresiva a iguales límites y mostrar sensibilidad. Los eventos/fechas posteriores reservados no se usan para ajustar parámetros. Siguen pendientes selección económica óptima del universo, asignación conjunta de capital y calibración de fills. La precompilación de dependencias y patas conserva la salida de la captura original de dos horas; en el benchmark reproducible de 48 mercados reduce un 6,50 % la mediana del tiempo del estudio en este M2. Es una medición de ingeniería, no de ingresos; véase [PERFORMANCE.md](PERFORMANCE.md).

## Decisión de la revisión base — 15 de septiembre de 2026

La siguiente etapa debe mejorar **qué operaciones elige el motor, cuánto compra y cómo completa la cartera**. La base de arbitraje estructural sigue siendo una hipótesis razonable: adquirir posiciones cuyo pago mínimo conjunto supere su coste completo. Su rentabilidad en Kalshi permanece **indeterminada**. La revisión no convierte esa incertidumbre en una previsión de pérdidas ni en una promesa de beneficios.

La prioridad inmediata seleccionada en el plan es un dimensionador que maximice el margen neto entre tamaños financiables, acompañado de observación de más familias. Después se completa la comparación de políticas de ejecución. Las cestas de varias patas se evaluarán si los datos identifican una oportunidad que justifique ampliar el alcance. La velocidad se optimizará dentro de esas decisiones, midiendo también su efecto económico.

Hay una evidencia local nueva que hace concreta esta prioridad: en un ejemplo sintético reproducido con el ejecutable actual, dos contratos por pata dejan un margen de **0,1370 USD** después de las comisiones supuestas, mientras que la política con límite de cinco contratos rechaza toda la oportunidad. El mecanismo actual busca el máximo tamaño financiable y puede omitir uno menor rentable. Esto confirma una limitación del algoritmo; no es una operación de mercado real.

La ambición de trabajar como una firma cuantitativa se traduce aquí en mejorar la selección económica, representar bien las restricciones y ejecutar con poca latencia y errores controlados. Los materiales públicos de las firmas permiten aprender métodos de ingeniería; no revelan una estrategia comercial completa que podamos copiar con los mismos resultados.

## Alcance, método y estado de partida

Se revisaron trabajos originales, copias de autores y documentación de Kalshi. La búsqueda siguió cuatro preguntas: dónde aparecen desajustes pertinentes, cómo escoger cantidades, cómo gestionar ejecuciones incompletas y cuándo compensa reducir latencia o inmovilizar capital. Se siguieron referencias entre artículos y se compararon sus definiciones de oportunidad y beneficio.

Los resultados empíricos de Polymarket son evidencia sobre ese entorno. Los modelos teóricos aportan herramientas condicionadas a sus supuestos. Las decisiones descritas como **propuestas** son adaptaciones para este proyecto; ningún paper citado acredita su rendimiento en nuestro motor. Los preprints se identifican como tales. Dos trabajos de SSRN quedaron limitados a sus resúmenes y no se usan como fundamento de una implementación.

El código inspeccionado corresponde a `7cf248f8fe96fd01091998a1fe642073d9c6bb9a`, base de trabajo de [PR #7](https://github.com/DamianAcri/event-market-execution-engine/pull/7). Esta referencia fija el estado analizado y no afirma que el PR esté fusionado. El detalle funcional se mantiene en [OFFLINE_STUDY.md](OFFLINE_STUDY.md).

| Capacidad en esa revisión | Implicación económica |
|---|---|
| Libros locales, reglas de validez, cantidades exactas y replay verificable | Permiten reconstruir decisiones sobre entradas explícitas. |
| Implicaciones y complementos de dos patas, con verificación de pagos | Cubren relaciones estructurales concretas; no todas las carteras rentables. |
| Evaluación incremental y consulta de profundidad sin copiar el libro | Reducen trabajo repetido en búsqueda y costes. |
| Comisiones por orden, reservas y simulación IOC con llegadas separadas | Permiten estudiar cantidades, costes y carteras incompletas bajo supuestos declarados. |
| Un intento por restricción y tamaño máximo financiable | Política de referencia que puede omitir tamaños menores y episodios posteriores. |
| Sin transporte de órdenes, confirmaciones, liquidación efectiva ni deshacer posiciones | Todavía no existe un ciclo completo de ejecución ni beneficio realizado. |

El piloto observado anterior contiene 96 snapshots REST de ocho contratos de un solo evento durante aproximadamente 75 segundos, con cero candidatos brutos. Es un resultado pequeño y local: no identifica la capacidad económica del universo de Kalshi. Los ejemplos sintéticos verifican escenarios del simulador; no sustituyen datos reales ni demuestran que la estrategia gane o pierda.

## Qué aporta la literatura

### Arbitraje combinatorio: una línea pertinente, con estimaciones diferentes

**Saguillo, Ghafouri, Kiffer y Suarez-Tangil (2025), preprint.** Clasifican arbitrajes y combinan filtrado semántico, modelos de lenguaje y revisión manual para localizar relaciones. Su reconstrucción usa operaciones on-chain, VWAP por bloque y arrastre de precios; no es un histórico íntegro de profundidad ejecutable. La utilidad para Calci es organizar la búsqueda y revisar relaciones candidatas. Sus cifras agregadas no son ingresos accesibles a un nuevo bot. Un resultado agregado «OTHER» tampoco proporciona por sí mismo un instrumento comprable.[^saguillo]

**Gebele, Mutzel y Matthes (2026), preprint.** Separan equivalencia de pagos de mecanismos que permiten realizarla, y encuentran una concentración de su beneficio estimado en conversiones de Polymarket. Parte de la valoración de posiciones es imputada; los paneles de libros y transacciones no coinciden temporalmente. La comparación con Saguillo utiliza criterios de atribución distintos. El mensaje transferible es comprobar el mecanismo y la financiación de cada cartera, no trasladar el beneficio estimado ni las conversiones a Kalshi.[^executable]

**Cheng, Yang y Zou (2026), preprint.** Estudian relaciones entre ganador y hándicap en NBA, próximas a nuestras implicaciones. Encuentran episodios combinatorios y poca profundidad. El muestreo tiene intervalos de varios segundos y sus cálculos suponen comisiones de negociación nulas. El apéndice A.5 selecciona el máximo beneficio de cada episodio a posteriori.[^nba]

**Nuestra lectura crítica:** ese máximo sirve como referencia retrospectiva, pero no como política implementable que supiera cuándo entrar. Arrastrar una cotización observada tampoco demuestra que siguiera disponible. Adoptaremos la relación entre contratos como hipótesis de búsqueda, usando llegada causal, costes propios y ejecución incompleta. No tomaremos sus duraciones como presupuesto de latencia ni sus beneficios como previsión de Calci.

### Optimización matemática: herramientas para seleccionar carteras

**Kroer, Dudík, Lahaie y Balakrishnan (ACM EC 2016).** Representan resultados válidos mediante programación entera y usan un oráculo dentro de Frank–Wolfe para un creador de mercado basado en una función de coste. Es un modelo de formación de precios distinto de operar contra un libro. Aporta representación de restricciones y verificación de pagos combinatorios; no entrega directamente un dimensionador de órdenes para Kalshi.[^kroer]

**Aplicación propuesta:** conservar un oráculo independiente para verificar pagos y cantidades en casos pequeños. Usar optimización general fuera del camino por actualización como referencia cuando aumenten las patas. Después implementar algoritmos especializados para las familias frecuentes, contrastados con ese oráculo. La necesidad de un solver debe aparecer en un problema concreto antes de incorporarlo a la arquitectura.

### Ejecución: tipo de orden y riesgo de quedar a medias

**Cont y Kukanov, Quantitative Finance, versión de autores en arXiv.** Formulan la elección entre órdenes agresivas y pasivas con colas, comisiones y penalizaciones por exceso o defecto de ejecución. El problema es comprar el mismo activo en varias bolsas; presupone disponibilidad inmediata de la orden agresiva de recuperación. Esa última condición no puede asumirse para nuestros contratos.[^cont]

**Lehalle y Mounjid.** Su modelo y datos de renta variable conectan desequilibrio del libro, selección adversa y valor de cancelar/reubicar órdenes con latencia. Sirve para formular políticas condicionadas al estado. No calibra probabilidades de ejecución para Kalshi ni demuestra independencia entre nuestras patas.[^lehalle]

**Feil y Nendel (2026), preprint.** Modelan cotización e inventario con precios binarios, riesgo durante la negociación y condición terminal. Sus parámetros numéricos son ilustrativos, no calibrados a un contrato. Es una referencia para una etapa posterior de cotización pasiva o inventario residual, no una prueba de ventaja económica actual.[^feil]

**Aplicación propuesta:** comenzar por decisiones conjuntas sencillas —entrar, completar, reducir o abandonar—, limitadas por fondos y exposición residual. Medir la utilidad de añadir complejidad frente a esa referencia. No asumir que una orden pendiente se ejecuta ni que una cobertura siempre está disponible.

### Capital y retornos históricos

**Gebele y Matthes (2026), preprint sobre liquidación.** Estudian descuentos asociados al tiempo hasta recuperar el capital. El descuento estimado mezcla fricciones y riesgo residual; no equivale a un tipo libre de riesgo. Su selección de contratos persistentemente próximos a certeza excluye reversiones posteriores, y emplea tiempos realizados de liquidación como aproximación.[^capital]

**Aplicación propuesta:** medir el tiempo hasta efectivo reutilizable y estimarlo solo con información disponible al decidir. No convertir «comprar a 0,99» en arbitraje certificado ni seleccionar retrospectivamente contratos que nunca revirtieron. Una cartera con garantía de pago puede bloquear capital durante demasiado tiempo para nuestro presupuesto.

**Bürgi, Deng y Whelan (2026), working paper de Kalshi.** Documentan sesgos de precios y diferencias entre makers y takers en una muestra que termina en abril de 2025, excluye contratos abiertos menos de 24 horas y usa el régimen histórico sin comisiones maker. Es relevante para hipótesis de selección y ejecución; no demuestra el beneficio actual de una nueva estrategia pasiva.[^makers]

## 1. Buscar donde las reglas permitan carteras útiles

Proponemos organizar el universo por **plantillas de pago verificables**. Popularidad y volumen pueden ayudar a priorizar observación, pero no sustituyen las reglas de resolución.

| Familia candidata | Relación a verificar | Experimento propuesto |
|---|---|---|
| Umbrales del mismo indicador y vencimiento | Superar un umbral alto implica superar uno bajo, con iguales fuente y reglas excepcionales | Varias escaleras y fechas; localizar margen por profundidad y tamaño. |
| Ganador y margen deportivo | Satisfacer el margen implica el resultado ganador definido por ambos contratos | Revisar prórrogas, suspensión, aplazamiento y empates; separar fases del evento. |
| Intervalos o categorías que cubren un resultado | Al menos una categoría paga en todo estado admisible | Cestas de YES cuyo pago mínimo exceda el coste de todas sus patas. |
| Resultados mutuamente excluyentes | Como máximo uno paga YES | Cestas de NO, con límites de financiación y número de patas. |

Son familias a localizar y revisar, no una afirmación de que todos esos productos estén disponibles o sean rentables hoy en Kalshi. Antes de admitir una relación se fijarán textos de resolución, fuente, horizonte, excepciones, versión y procedencia. Un clasificador semántico puede proponer candidatos fuera del proceso de negociación; no emitirá garantías por similitud de títulos. Este flujo adapta la búsqueda temática de Saguillo.[^saguillo]

Hay una ampliación matemática concreta más allá de dos patas. Si tres resultados son mutuamente excluyentes, comprar un NO de cada uno paga **al menos 2 USD**. Un ejemplo construido con precios 0,60 + 0,60 + 0,60 cuesta 1,80 USD antes de comisiones. Ninguna pareja de esas posiciones ofrece el mismo margen: cuesta 1,20 USD y su pago mínimo es 1 USD. Un detector limitado a parejas puede omitir esa cesta aunque procese correctamente cada componente.

No confundir exclusividad y exhaustividad. Para todos los YES, «como máximo uno gana» no garantiza que alguno gane. Para una cesta de `n` NO, la exclusividad basta para un mínimo `n−1` bajo resolución binaria ordinaria; anulaciones o pagos distintos deben entrar en el oráculo. El modelo de resultados, no el nombre comercial del grupo, determina la garantía.

Inicialmente limitaremos patas y complejidad por familia. Más combinaciones amplían la búsqueda, pero también pueden aumentar comisiones, mensajes y exposición durante la adquisición.

## 2. Elegir el tamaño por beneficio neto

Para una cartera equilibrada de dos patas y cantidad `q`, definimos:

```text
G(q) = pago mínimo certificado(q)
       − coste de profundidad de ambas patas(q)
       − comisiones y redondeos de adquisición(q)
       − costes incrementales declarados(q)

q* = argmax G(q), incluyendo q = 0,
     entre cantidades admitidas y financiables durante toda la adquisición
```

Este objetivo es **margen al adquirir completamente la cartera**, no beneficio esperado con ejecuciones parciales. Es el primer escalón antes de optimizar una política con incertidumbre de ejecución. Los costes generales fijos se deducen al evaluar la actividad total, sin multiplicarlos artificialmente por cada candidato.

### Reproducción con el ejecutable actual

Se construyó una sesión sintética con una implicación `A ⇒ B`. Comprar NO de A cuesta 0,30 USD por contrato hasta cinco contratos. Comprar YES de B cuesta 0,60 USD para los dos primeros y 0,80 USD para los tres siguientes. Ambos libros permanecen válidos, la llegada es inmediata y hay 100 USD disponibles.

Supuestos: cantidades enteras, coeficiente cuadrático 0,07, precisión de saldo 0,0001 USD, una ejecución por nivel de precio, sin costes operativos ni crédito de colateral. No representan comisiones calibradas de una cuenta real. La fórmula y los ajustes de la API deben verificarse para cada integración.[^fees]

Se ejecutaron cinco estudios, cambiando únicamente el límite de cantidad. La aritmética de referencia se calculó por separado con `Decimal` y coincidió con los débitos y márgenes de las tres operaciones admitidas:

| Cantidad por pata | Coste sin comisiones (USD) | Comisiones supuestas (USD) | Margen si se compra esa cantidad completa (USD) | Política actual con ese límite |
|---:|---:|---:|---:|---|
| 1 | 0,9000 | 0,0315 | +0,0685 | Completa una pareja. |
| 2 | 1,8000 | 0,0630 | **+0,1370** | Completa dos. |
| 3 | 2,9000 | 0,0889 | +0,0111 | Completa tres, con menos margen total. |
| 4 | 4,0000 | 0,1148 | −0,1148 | Rechaza; no busca cantidades menores. |
| 5 | 5,1000 | 0,1407 | −0,2407 | Rechaza; no busca cantidades menores. |

Los valores negativos de la cuarta columna son cálculos de una compra hipotética; **el motor no ejecuta esas operaciones ni pierde ese dinero**. Sus resultados son cero intentos y saldo sin cambios. En todos los estudios el campo de beneficio realizado conserva el valor `null`: no se ha calculado un beneficio realizado. La cantidad óptima entre los tamaños enteros del ejemplo es dos, utilizando únicamente información presente en el libro.

### Cambio recomendado y optimización

Crear una referencia exhaustiva sobre la rejilla de cantidades permitida, con límite de trabajo explícito. Incluir fondos, límites de precio, profundidad, redondeos y la opción de no operar. Conservar la política anterior para comparar. No usar búsqueda binaria sobre el signo del beneficio: su monotonicidad no está garantizada. Tampoco afirmar que basta evaluar extremos de niveles cuando los redondeos pueden modificar el óptimo.

Después reducir recorridos mediante costes acumulados y reutilización del cálculo por pata. Cualquier poda necesitará una cota válida para comisiones y rejilla. Si el espacio excede el presupuesto computacional, emitir que la búsqueda quedó limitada; una aproximación no debe presentarse como óptima. Mantener el cálculo independiente como referencia en tamaños pequeños.

La aceptación tiene dos componentes: coincidencia de cantidad, margen y reserva con la referencia; y coste de cálculo medido al variar niveles, cantidades y fraccionamiento. Elegir mejor en un ejemplo no acredita todavía mayor beneficio real ni una aceleración del ejecutable.

## 3. Repartir capital entre oportunidades que compiten

Maximizar cada candidato aisladamente puede gastar dos veces la misma profundidad o reservar capital para una oportunidad peor. Proponemos seleccionar conjuntamente dentro de componentes que compartan mercados, liquidez o límites de riesgo, respetando además el presupuesto global.

Como referencia estática, sea `x` un vector de cantidades, `W` los resultados válidos y `C(x)` el coste total sobre profundidad compartida:

```text
maximizar z
sujeto a z ≤ pago(x, w) − C(x) para todo w en W
         consumo de cada nivel ≤ profundidad disponible
         cantidades sobre su rejilla
         financiación máxima del camino de adquisición ≤ efectivo disponible
         límites de exposición y número de órdenes
```

Esta es una **formulación propuesta para Calci**, inspirada en la representación combinatoria de resultados, no un algoritmo de ejecución publicado por Kroer. Con costes simplificados puede construirse una relajación; con cantidades, redondeos y caminos de adquisición exactos hay decisiones discretas. Resolver una relajación no certifica automáticamente una cartera enviable.

El oráculo estático mide margen omitido por una política sencilla en un instante. No debe usar capital de liquidaciones futuras ni convertirse en selección retrospectiva de las mejores operaciones del día. El problema secuencial depende también de cuándo vuelve el efectivo y qué otras oportunidades aparecen.

Mediremos beneficio neto, pico de fondos y capital por tiempo. Este último puede calcularse como la integral del capital inmovilizado, distinta del nominal negociado. Beneficio/capital-tiempo es descriptivo; puede ser una mala regla única con pérdidas de cola o restricciones comunes.

Para comparar **100 €, 1.000 € y 10.000 €**, cada estudio futuro fijará conversión EUR/USD, fecha y costes aplicables. No se ha hecho esa conversión en este ejemplo, denominado en USD. El efectivo adicional puede quedar ocioso si la profundidad útil o las oportunidades simultáneas son pequeñas; no se extrapolarán beneficios proporcionalmente al presupuesto.

## 4. Decidir cómo completar todas las patas

Una garantía de pago de la cartera completa no elimina el riesgo de adquirir solo una parte. Kalshi V2 admite IOC y FOK por orden; el lote devuelve resultados por orden y no promete adquisición atómica entre mercados. La política debe contemplar resultados diferentes entre patas.[^orders][^batch]

Proponemos comparar, bajo los mismos límites:

| Política experimental | Decisión económica que permite estudiar | Trabajo que requiere |
|---|---|---|
| IOC de ambas patas con llegadas separadas | Referencia de captura agresiva y pérdidas residuales | Base actual; añadir respuesta y recuperación. |
| Envío secuencial con reevaluación | Si el riesgo de la primera pata compensa esperar antes de la segunda | Confirmaciones, pendientes y cobertura con precios posteriores disponibles. |
| FOK por pata | Si reducir parciales dentro de cada orden mejora el conjunto | Semántica validada y posibilidad de completar solo una pata. |
| Primera pata pasiva y cobertura posterior | Si el precio obtenido compensa espera y selección adversa | Colas, cancelaciones en vuelo, ejecuciones propias y cobertura. |

El orden por identificador de mercado actual es determinista, pero no expresa una preferencia económica. Empezar por la pata con menos liquidez es una hipótesis, no una verdad general: adquirirla puede dejar una cobertura muy cara si el resto cambia.

El objetivo posterior será beneficio neto esperado sujeto a límites de pérdida, financiación y tiempo expuesto. La probabilidad relevante es conjunta y condicionada al estado de ambos libros; multiplicar probabilidades independientes puede ocultar su dependencia. Precio, profundidad, cambios recientes, antigüedad y fase del evento son candidatos a variables explicativas. Usar primero modelos sencillos y calibrados antes de redes o control estocástico complejo.

Hasta disponer de observaciones adecuadas, los supuestos se mostrarán como escenarios: cambios adversos coordinados, una pata rechazada, desaparición de profundidad o cobertura imposible. Una penalización supuesta no es una pérdida estimada del mercado. Una política con inventario residual debe definir cuándo cubrir, cuánto gastar y qué hace si no hay salida financiable.

La devolución de colateral es otra transición explícita. Kalshi documenta elegibilidad por evento, configuración y restricciones posteriores a la venta. No podemos derivar su disponibilidad solo de la equivalencia matemática de pagos. Un estado desconocido de cuenta no aportará financiación implícita.[^collateral]

## 5. Optimizar búsqueda y latencia con un objetivo económico

Los materiales de Optiver resaltan la relación entre iteración, abstracciones y comportamiento de los sistemas. Jane Street muestra cálculo incremental y técnicas para investigar variación de latencia por observación. Son métodos aplicables; sus cifras de hardware no fijan una meta de rendimiento para Calci.[^optiver][^incremental][^jitter]

### Propuesta algorítmica para escaleras de umbrales

En una familia verificada, ordenar umbrales de menor a mayor. Para índices `i < j`, comprar YES del umbral bajo y NO del alto tiene margen bruto unitario:

```text
bid_yes[j] − ask_yes[i]
```

Un árbol de segmentos puede guardar el menor ask, el mayor bid y la mejor diferencia válida. Al unir segmentos contiguos L y R:

```text
best = max(L.best, R.best, R.max_bid − L.min_ask)
```

Cualquier pareja admisible está dentro de L, dentro de R o cruza desde L a R. Las hojas no admiten una pareja consigo mismas. Un cambio afecta O(log n) nodos para mantener el **mejor margen bruto**, con estructura O(n). Es una propuesta propia especializada, no un resultado de rentabilidad extraído de una firma.

Con comisiones no negativas, un máximo bruto no positivo permite descartar esa familia para estas parejas. Un máximo positivo requiere comprobar profundidad, comisiones y fondos. Si la mejor pareja bruta no es financiable hay que buscar otras; detenerse puede omitir una oportunidad. Las invalidaciones por tiempo también deben actualizar la estructura aunque no llegue un cambio de precio.

Este resumen no sustituye automáticamente al contrato actual que emite eventos por candidato. Enumerar todas las parejas puede tener salida cuadrática. Antes de adoptarlo hay que comparar contra enumeración exhaustiva, preservar la semántica requerida y demostrar beneficio con familias suficientemente grandes. Para familias pequeñas, el índice incremental existente puede ser preferible.

### Presupuesto de latencia y hardware

Separar recepción, espera, decodificación, libro, búsqueda, tamaño, preparación, envío y respuesta. Perfilar también ráfagas y colas de latencia; una media de replay completo no mide llegada al mercado.

El experimento económico variará esos retrasos sobre sesiones con resolución temporal suficiente y política fija. Comparará el beneficio incremental simulado con el coste adicional de infraestructura y la incertidumbre de ejecución. La curva decide qué tramo optimizar; no se obtiene con precisión submilisegundo a partir de snapshots REST espaciados segundos.

Mantener C++ portable, memoria acotada y perfiles por arquitectura. Priorizar aritmética exacta, costes acumulados e índices antes de SIMD específico, aislamiento de núcleos o tecnologías de red adicionales. Recomendar hardware cuando midamos qué tramo limita oportunidades y cuánto cuesta reducirlo. Las mejoras previas en M2 no acreditan el mismo rendimiento en Windows o Linux.

## Experimentos y orden de implementación propuestos

Los identificadores E1–E5 son propuestas de investigación, no cinco fases obligatorias. El [plan vigente](IMPLEMENTATION_PLAN.md) selecciona E1 como P1 y observación E2 como P2; la ejecución E4 alimenta P3 y la medición E5 acompaña los cambios. E3 es una extensión condicionada a oportunidades observadas o restricciones relevantes. La siguiente tabla conserva qué pretende comprobar cada experimento, sin definir un orden paralelo.

| Experimento | Trabajo | Resultado que decidirá si mejora el proyecto |
|---|---|---|
| E1 | Tamaño que maximiza margen neto; oráculo exacto y profundidad acumulada | Recuperar tamaños rentables omitidos y coincidir con la referencia. Medir tiempo y memoria. |
| E2, junto a E1 | Universo revisado y sesiones de varias fechas, estados y familias | Identificar familias con margen después de comisiones y límites que impiden capturarlo. |
| E3 | Cestas acotadas de varias patas y asignación entre candidatos | Medir margen adicional frente a parejas bajo igual profundidad, financiación y riesgo. |
| E4 | Confirmaciones simuladas, cobertura residual y políticas conjuntas | Mejorar resultado neto con pérdidas y exposición limitadas, incluyendo intentos fallidos. |
| E5, durante E2–E4 | Sensibilidad a retrasos y perfilado de los tramos relevantes | Relacionar menor latencia con capturas adicionales y coste operativo. |

Cada experimento usará una referencia simple y cambiará una decisión principal. Se reservarán fechas y eventos antes de ajustar parámetros. La unidad económica será oportunidad/episodio y evento, además de operaciones; miles de actualizaciones correlacionadas no equivalen a oportunidades independientes.

El tracker ya describe aperturas e invalidaciones brutas. Un episodio económico depende también de tamaño, coste, fondos y estado ejecutable. Reintentar requerirá una regla causal y un modelo explícito de reposición; no bastará contar cada snapshot como liquidez nueva. El máximo retrospectivo de un episodio podrá informarse como referencia separada, nunca como captura.

Los informes mostrarán margen descartado por reglas, profundidad, coste, financiación y ejecución; resultado neto; exposición residual; y concentración por evento. Se respetarán dependencias temporales y registrarán todas las variantes probadas. Seleccionar entre muchos ensayos exige corregir el optimismo estadístico, pero ninguna corrección arregla ejecuciones imposibles.[^dsr]

E1 y gran parte de E3 pueden desarrollarse sin claves. REST público permite ampliar familias, con sus límites temporales. Captura WebSocket autenticada y calibración con mensajes de órdenes son pasos distintos; no quedan sustituidos por este documento ni por el simulador. No se han usado credenciales ni enviado órdenes durante esta investigación.

## Qué investigación queda abierta

No hace falta cerrar toda la literatura antes de E1. Sí quedan preguntas capaces de cambiar la estrategia:

1. **Margen accesible hoy en Kalshi:** frecuencia por familia, profundidad útil, episodios en noticias y vencimientos. Se resolverá principalmente con datos propios y términos exactos, no extrapolando otra plataforma.
2. **Coste conjunto de ejecutar y cubrir:** fills propios, fraccionamiento, cancelación, latencia y cambio adverso del resto de la cartera. Falta para elegir entre agresivo, secuencial y pasivo.
3. **Capital reutilizable:** liquidación y caminos de colateral según evento y cuenta; impacto sobre carteras simultáneas.
4. **Señales informativas para ejecución:** flujo y liderazgo entre mercados podrían ayudar a evitar cotizaciones obsoletas. Añadir predicción direccional cambiaría el riesgo y debe compararse como extensión separada del arbitraje certificado.

Ng, Peng, Tao y Zhou, *Price Discovery and Trading in Modern Prediction Markets*, queda pendiente de texto completo. El resumen señala liderazgo variable por categoría y actividad; no basta para diseñar una señal operable. Las páginas de autores conservan también una descripción anterior: hay que fijar la versión antes de reproducir resultados.[^ng]

Yang, *Skilled Liquidity Provision in Prediction Markets*, queda igualmente pendiente. Solo se pudo revisar el resumen, que distingue habilidad de participación maker/taker en Polymarket. No se adopta una regla de negociación ni una cifra de beneficio a partir de él.[^yang]

SSRN devolvió acceso denegado al intentar recuperar los textos. Esta limitación afecta a esas dos lecturas, no al resto de la revisión. No se ha afirmado haber replicado los análisis empíricos de ningún paper.

## Contribución de un colaborador de economía

Puede comparar términos de resolución y excepciones; proponer familias y ventanas de observación; reconstruir financiación y rentabilidad por tiempo; y cuestionar si un beneficio aparente depende de supuestos optimistas. Un primer entregable útil sería un conjunto pequeño de relaciones justificadas y criterios acordados antes de mirar resultados. Su contribución complementa la implementación cuando produce hipótesis comprobables o descubre restricciones que cambian la decisión de operar.

## Límites de la conclusión y trazabilidad

La revisión respalda continuar por búsqueda, tamaño y ejecución con prioridades más concretas. Hay métodos pertinentes y limitaciones corregibles, pero todavía no sabemos si la frecuencia y margen capturables cubrirán costes y pérdidas con nuestro capital. Tampoco hemos probado que no puedan hacerlo. Si una familia solo resulta atractiva con cotizaciones inexistentes o sin costes, se descarta esa hipótesis; no se atribuye ese resultado a todo el proyecto.

La ejecución nueva de esta revisión consiste en los cinco estudios sintéticos de cantidad, no en una prueba de rentabilidad ni un nuevo benchmark de velocidad. El paquete local `calci-economic-research-20260915` conserva `reproduce_sizing.py`, entradas, sesión verificada, cinco políticas, cinco salidas JSONL, `results.json` y manifiesto con hashes del ejecutable y archivos. El script recibe un ejecutable existente y un directorio nuevo; no modifica código ni contacta con la plataforma. El documento se guarda también en ese paquete.

## Fuentes

Fuentes de la revisión base consultadas el 14–15 de septiembre de 2026; las añadidas en la actualización se consultaron el 17. Las secciones fijan el alcance utilizado; acceso al texto completo no implica reproducción de sus resultados.

[^nflwinner]: Kalshi. [FOOTBALLGAMEWIN](https://assets.kalshi.com/contract_terms/FOOTBALLGAMEWIN.pdf). Términos archivados el 17 de septiembre de 2026, SHA-256 `19578b71cd63da63cc2894c60542ef06bb5de56b9366add56f07cee645cfca4b`: empate, suspensión, aplazamiento, forfeit y cambios de sede.
[^nflspread]: Kalshi. [FOOTBALLSPREAD](https://assets.kalshi.com/contract_terms/FOOTBALLSPREAD.pdf). Términos archivados el 17 de septiembre de 2026, SHA-256 `1daaea87a853adcb485bdb80fb8bd5c5fa7c831d8e237f7446bc16ea43705f5a`: márgenes, prórrogas y liquidaciones excepcionales.
[^queue]: Weibing Huang, Charles-Albert Lehalle y Mathieu Rosenbaum. [Simulating and analyzing order book data: The queue-reactive model](https://arxiv.org/html/1312.0563). Versión de autores, v2, 2014; Lehalle afiliado a Capital Fund Management. Sección 2.5: prioridad, cancelaciones y límite informativo sin identificadores de órdenes. Modelo de renta variable, no calibración de Kalshi.
[^trades]: Kalshi. [Public Trades](https://docs.kalshi.com/websockets/public-trades), [Order direction](https://docs.kalshi.com/getting_started/order_direction) y [AsyncAPI](https://docs.kalshi.com/asyncapi.yaml), consultados el 17 de septiembre de 2026. `tradePayload`, `sequenceNumber` y `is_block_trade`; dirección del taker y precios expresados en YES.

[^saguillo]: Oriol Saguillo, Vahid Ghafouri, Lucianna Kiffer y Guillermo Suarez-Tangil. [Unravelling the Probabilistic Forest: Arbitrage in Prediction Markets](https://arxiv.org/html/2508.03474v1). Preprint, 2025, v1. Secciones 4–7: búsqueda, reconstrucción y atribución.
[^executable]: Jonas Gebele, Timm Mutzel y Florian Matthes. [Executable Arbitrage and Market Efficiency in Prediction Markets](https://arxiv.org/html/2608.00666v1). Preprint, agosto de 2026, v1. Secciones 4–7 y apéndice A: paneles, imputaciones y mecanismos.
[^nba]: Guang Cheng, Jiaxin Yang y Haoxuan Zou. [Arbitrage Analysis in Polymarket NBA Markets](https://arxiv.org/html/2605.00864v1). Preprint, 2026, v1 consultada. Secciones 2.3, 4–5; apéndices A.3–A.5 y B: muestreo, costes, criterio retrospectivo y alineación.
[^kroer]: Christian Kroer, Miroslav Dudík, Sébastien Lahaie y Sivaraman Balakrishnan. [Arbitrage-Free Combinatorial Market Making via Integer Programming](https://www.columbia.edu/~ck2945/papers/milp_market.pdf). ACM EC 2016, DOI 10.1145/2940716.2940767. Modelo, representación y oráculo IP.
[^cont]: Rama Cont y Arseniy Kukanov. [Optimal Order Placement in Limit Order Markets](https://arxiv.org/pdf/1210.1625). Copia de autores del artículo de Quantitative Finance, 17(1), 21–39; DOI 10.1080/14697688.2016.1190030. Secciones 1–2. La copia lleva también el título «Optimal order placement and routing in limit order markets».
[^lehalle]: Charles-Albert Lehalle y Othmane Mounjid. [Limit Order Strategic Placement with Adverse Selection Risk and the Role of Latency](https://arxiv.org/html/1610.00261). Primera circulación, 2016. Secciones 2, 4 y 5: renta variable y control.
[^feil]: Dominik Feil y Max Nendel. [Optimal Market Making in Prediction Markets](https://arxiv.org/html/2607.17991v1). Preprint, julio de 2026. Secciones 3 y 4.1–4.2: control e ilustración sin calibración específica.
[^capital]: Jonas Gebele y Florian Matthes. [When Certainty Is Not Worth It: Capital Lock-Up and Settlement Discounting in Prediction Markets](https://arxiv.org/html/2605.31431v1). Preprint, mayo de 2026. Secciones 3 y 4.1–4.2: modelo, selección e identificación.
[^makers]: Constantin Bürgi, Wanying Deng y Karl Whelan. [Makers and Takers: The Economics of the Kalshi Prediction Market](https://www2.gwu.edu/~forcpgm/2026-001.pdf). Working paper, enero de 2026; portada de distribución de febrero, número 2026-001. Secciones 2, 3.1 y 6: muestra, comisiones y límites.
[^fees]: Kalshi. [Fee Rounding](https://docs.kalshi.com/getting_started/fee_rounding). Precisión, cargos y acumulador por orden. El coeficiente del ejemplo es un supuesto, no una tarifa descubierta para una cuenta.
[^orders]: Kalshi. [Create Order (V2)](https://docs.kalshi.com/api-reference/orders/create-order-v2). Precio YES, cantidades y opciones `time_in_force`.
[^batch]: Kalshi. [Batch Create Orders (V2)](https://docs.kalshi.com/api-reference/orders/batch-create-orders-v2). Resultados por orden y límites; no garantiza atomicidad entre mercados.
[^collateral]: Kalshi. [Collateral Return](https://help.kalshi.com/en/articles/13823816-collateral-return). Configuración, elegibilidad y restricciones; sin inspección de cuenta.
[^optiver]: Daniel B., Optiver. [Designing for latency and research iteration](https://www.optiver.com/insights/technology-blog/designing-for-latency-and-iteration/). Julio de 2026. Relato técnico de primera mano; resultados cualitativos.
[^incremental]: Yaron Minsky, Jane Street. [Seven Implementations of Incremental](https://www.janestreet.com/tech-talks/seven-implementations-of-incremental/). Charla y transcripción oficiales sobre cálculo incremental.
[^jitter]: Tudor Brindus, Jane Street. [System Jitter and Where to Find It: A Whack-a-Mole Experiencer](https://www.janestreet.com/tech-talks/system-jitter-and-where-to-find-it/). Charla y transcripción oficiales sobre variación de latencia.
[^dsr]: David H. Bailey y Marcos López de Prado. [The Deflated Sharpe Ratio: Correcting for Selection Bias, Backtest Overfitting and Non-Normality](https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf). Versión de autores, 31 de julio de 2014, The Journal of Portfolio Management. Selección entre ensayos; ya incluida en QUANT_RESEARCH.md.
[^ng]: Hunter Ng, Lin Peng, Yubo Tao y Dexin Zhou. [Price Discovery and Trading in Modern Prediction Markets](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5331995). Working paper, revisión indicada por SSRN en septiembre de 2026. **Solo resumen y metadatos accesibles**; páginas de [Yubo Tao](https://sites.google.com/site/ybtao1990/research) y [Hunter Ng](https://hunterng.com/research/). Pendiente fijar y leer texto completo.
[^yang]: Hsiang-Chieh (Alex) Yang. [Skilled Liquidity Provision in Prediction Markets: Evidence from 150 Million Trades](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6556613). Working paper, registro de abril de 2026. **Solo resumen accesible**; pendiente texto y método.
