macroScript OpenNovaExportAse category:"OpenNova" tooltip:"Export Novalogic ASE"
(
    on execute do
    (
        python.Execute "from opennova_max import ui; ui.export_ase()"
    )
)

macroScript OpenNovaExportAnims category:"OpenNova" tooltip:"Export Novalogic ADM + BAD animations"
(
    on execute do
    (
        python.Execute "from opennova_max import ui; ui.export_anims()"
    )
)
