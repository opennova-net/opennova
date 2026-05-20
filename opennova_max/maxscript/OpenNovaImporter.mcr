macroScript OpenNovaImporter category:"OpenNova" toolTip:"OpenNova Importer"
(
    on execute do python.execute "import opennova_max.ui as ui; ui.show_importer()"
)

macroScript OpenNovaExportAse category:"OpenNova" toolTip:"Novalogic ASE Export"
(
    on execute do python.execute "import opennova_max.ui as ui; ui.export_ase()"
)

macroScript OpenNovaExportAnims category:"OpenNova" toolTip:"Novalogic Animation Export"
(
    on execute do python.execute "import opennova_max.ui as ui; ui.export_anims()"
)
