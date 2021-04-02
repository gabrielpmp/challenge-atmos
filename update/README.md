Update app - v1.0.2
====================

Esta pasta contém a versão do app para realização do update via cartão SD. O app é responsável por verificar se a atualização na partição OTA foi bem sucedida e apagar a versão já instalada do update. Caso contrário, o app realiza o rollback para a versão anterior da aplicação. O rollback pode ser testado conectando o GPIO 4 ao GND durante a função de *diagnostic()*, como no exemplo *.\esp-idf\examples\system\ota\native_ota_example* da IDF. Essa versão do app também pisca um LED ligado ao GPIO 2 por padrão para sinalizar o update do firmware.
